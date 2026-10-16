/*-------------------------------------------------------------------------
 *
 * refs_copies.c
 *	  Refs implementation "copies": the commits of a database whose branches
 *	  are copies of its versioned schemas (branch_ops.h), kept in its chunk
 *	  store.
 *
 * The branches are branch_ops' own; these refs give them head commits.
 * The chunk store's root is a tree keyed by memcmp:
 *
 *	'B' name	-> the branch's head commit (hash length, hash)
 *	'W' name	-> the branch's baseline: what its last fold for a commit
 *				   or a working-set checkpoint left, for the next fold to
 *				   start from (the canonicalizer's tree; hash length, hash),
 *				   then, if known, the working set's root that fold made
 *				   (hash length, hash)
 *	'M' name	-> the branch's merge in progress: the merged head and the
 *				   common ancestor (hash length, hash each), then the OIDs
 *				   of tables with unresolved conflicts (4 bytes each,
 *				   big-endian)
 *	'P' name	-> if that merge is a cherry-pick: the picked commit's
 *				   message
 *
 * A branch without an entry has no commits yet.  A transaction can't
 * commit a merge with unresolved conflicts it left, unless
 * dolt.allow_commit_conflicts is on.  A commit is a tree of its
 * own (keys by memcmp), and its id is that tree's root hash, so the same
 * commit content has the same id in every database:
 *
 *	"author"	-> the author
 *	"message"	-> the message
 *	"parents"	-> the parents' ids, each a length byte and the hash
 *	"root"		-> the commit's root (length byte, hash): the canonical
 *				   form of the branch's versioned schemas, folded
 *	"time"		-> microseconds since the Postgres epoch, 8 bytes big-endian
 *
 * A branch made at a commit starts as a copy of the session's branch,
 * moved to the commit (the rows that differ written, as a reset writes
 * them).
 *
 * A transaction that changes the refs first moves the store's root to
 * itself, which makes other movers wait until it ends; then each change
 * moves the root again, with the transaction.  So writers of refs take
 * turns, each builds on the last, and an aborted transaction's changes are
 * gone.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/refs_copies.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/prolly_builtins.h"
#include "access/prolly_canon.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_config.h"
#include "access/prolly_xact.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/branch_ops.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#define TAG_BRANCH		'B'
#define TAG_MERGE		'M'
#define TAG_BASELINE	'W'
#define TAG_PICK		'P'
#define TAG_MOVER		'X'	/* the transaction that last moved a head */

static bool ready = false;

/*
 * The branches whose merge states the transaction set (names, in
 * TopTransactionContext): checked when it commits or prepares, whichever
 * branch it is on then.
 */
static List *merge_changed = NIL;

static void refs_xact_callback(XactEvent event, void *arg);
static bool merge_state_of(const char *branch, ProllyMergeState *state);
static void merge_key(StringInfo key, const char *branch);
static bool copies_merge_state(ProllyMergeState *state);
static void copies_set_merge_state(const ProllyMergeState *state);
static bool copies_commit_working_set(const ProllyHash *commit_id,
									  ProllyNodeRef *root);
static const ProllyTreeRoutine *tree;
static ProllyTreeEnv env;

static void
refs_init(void)
{
	if (ready)
		return;
	tree = prolly_tree_lookup("cow");
	memset(&env, 0, sizeof(env));
	env.chunk_format = prolly_chunkformat_lookup("basic");
	env.chunker = prolly_chunker_lookup("bytes");
	env.hash = prolly_hash_lookup("sha256");
	env.chunk_store = prolly_chunk_store();
	env.chunks = PROLLY_CALL(env.chunk_store, open, MyDatabaseId,
							 MyDatabaseTableSpace);
	env.compare = prolly_bytes_compare;
	RegisterXactCallback(refs_xact_callback, NULL);
	ready = true;
}

static bool
name_listed(List *names, const char *name)
{
	ListCell   *lc;

	foreach(lc, names)
		if (strcmp(lfirst(lc), name) == 0)
			return true;
	return false;
}

/* Unresolved conflicts of a merge the transaction made aren't committed. */
static void
check_merge_conflicts(const char *what)
{
	ListCell   *lc;

	if (prolly_allow_commit_conflicts())
		return;
	foreach(lc, merge_changed)
	{
		ProllyMergeState ms;

		if (merge_state_of(lfirst(lc), &ms) && ms.ntables > 0)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("cannot %s a transaction with unresolved merge conflicts", what),
					 errdetail("Table \"%s\" has conflicts on branch \"%s\".",
							   get_rel_name(ms.tables[0]),
							   (char *) lfirst(lc)),
					 errhint("Resolve them with dolt_conflicts_resolve() first, or set dolt.allow_commit_conflicts to keep them.")));
	}
}

static void
refs_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			check_merge_conflicts("commit");
			break;
		case XACT_EVENT_PRE_PREPARE:
			check_merge_conflicts("prepare");
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
			merge_changed = NIL;
			break;
		default:
			break;
	}
}

static void
put_hash(StringInfo out, const ProllyHash *h)
{
	appendStringInfoChar(out, (char) h->len);
	appendBinaryStringInfo(out, h->bytes, h->len);
}

static const char *
get_hash(const char *p, const char *end, ProllyHash *h)
{
	memset(h, 0, sizeof(*h));
	if (p >= end || (uint8) p[0] > PROLLY_HASH_MAX_LEN ||
		end - p < 1 + (uint8) p[0])
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly refs hold an invalid hash")));
	h->len = (uint8) p[0];
	memcpy(h->bytes, p + 1, h->len);
	return p + 1 + h->len;
}

static void
ref_of(const ProllyHash *h, ProllyNodeRef *ref)
{
	memset(ref, 0, sizeof(*ref));
	ref->hash = *h;
	ref->location.blkno = InvalidBlockNumber;
}

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/* ------------------------------------------------------------------------
 * The refs tree
 * ------------------------------------------------------------------------
 */

/* The refs tree's root, as the transaction sees it; false if none yet. */
static bool
refs_root(ProllyNodeRef *root)
{
	ProllyHash	h;

	refs_init();
	PROLLY_CALL(env.chunk_store, root, env.chunks, &h);
	if (h.len == 0)
		return false;
	ref_of(&h, root);
	return true;
}

/*
 * Hold the refs until the transaction ends: move the root to itself (or,
 * in a new store, to an empty refs tree).  A mover that comes first is
 * waited for, and then built on.  (Holding them already, this moves the
 * root to itself again, which the transaction's move already is.)
 */
static void
lock_refs(void)
{
	refs_init();
	for (;;)
	{
		ProllyHash	cur;
		ProllyHash	next;

		PROLLY_CALL(env.chunk_store, root, env.chunks, &cur);
		next = cur;
		if (cur.len == 0)
		{
			ProllyTreeBuilder *b = PROLLY_CALL(tree, build_begin, &env,
											   CurrentMemoryContext);
			ProllyNodeRef empty;

			PROLLY_CALL(tree, build_finish, b, &empty);
			next = empty.hash;
		}
		if (PROLLY_CALL(env.chunk_store, move_root, env.chunks, &cur, &next))
			break;
	}
}

static bool
refs_get(const char *key, int keylen, StringInfo value)
{
	ProllyNodeRef root;
	ProllyKey	k = {(const uint8 *) key, keylen};

	if (!refs_root(&root))
		return false;
	return PROLLY_CALL(tree, get, &env, &root, &k, value);
}

/* Put (value NULL: delete) one entry, with the transaction. */
static void
refs_put(const char *key, int keylen, const StringInfoData *value)
{
	ProllyNodeRef root;
	ProllyNodeRef new_root;
	ProllyEdit	e;

	lock_refs();
	if (!refs_root(&root))
		elog(ERROR, "prolly refs have no root");
	e.key.data = (const uint8 *) key;
	e.key.len = keylen;
	e.is_delete = (value == NULL);
	e.value.data = value ? (const uint8 *) value->data : NULL;
	e.value.len = value ? value->len : 0;
	PROLLY_CALL(tree, apply, &env, &root, &e, 1, &new_root);
	if (!PROLLY_CALL(env.chunk_store, move_root, env.chunks, &root.hash,
					 &new_root.hash))
		elog(ERROR, "prolly refs moved while held");
}

/* ------------------------------------------------------------------------
 * Branches
 * ------------------------------------------------------------------------
 */

static void
branch_key(const char *name, StringInfo key)
{
	appendStringInfoChar(key, TAG_BRANCH);
	appendStringInfoString(key, name);
}

/* A branch's head commit; false if it has none. */
static bool
branch_head(const char *name, ProllyHash *head)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;

	initStringInfo(&key);
	initStringInfo(&v);
	branch_key(name, &key);
	found = refs_get(key.data, key.len, &v);
	if (found)
		(void) get_hash(v.data, v.data + v.len, head);
	else
		memset(head, 0, sizeof(*head));
	pfree(key.data);
	pfree(v.data);
	return found && head->len > 0;
}

/*
 * Make head (NULL: none) branch name's head commit, noting the transaction
 * that moved it (head_moved_unseen()).
 */
static void
branch_set_head(const char *name, const ProllyHash *head)
{
	StringInfoData key;
	StringInfoData v;
	TransactionId xid = head ? GetTopTransactionId() : InvalidTransactionId;

	initStringInfo(&key);
	initStringInfo(&v);
	branch_key(name, &key);
	if (head)
		put_hash(&v, head);
	refs_put(key.data, key.len, head ? &v : NULL);

	resetStringInfo(&key);
	resetStringInfo(&v);
	appendStringInfoChar(&key, TAG_MOVER);
	appendStringInfoString(&key, name);
	appendBinaryStringInfo(&v, &xid, sizeof(xid));
	refs_put(key.data, key.len, head ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
}

/*
 * Did a transaction the active snapshot doesn't see move branch name's
 * head?  (A committer that waited for the refs finds the head another
 * moved, while its snapshot, taken before, lacks that one's rows.)
 */
static bool
head_moved_unseen(const char *name)
{
	StringInfoData key;
	StringInfoData v;
	TransactionId xid = InvalidTransactionId;

	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_MOVER);
	appendStringInfoString(&key, name);
	if (refs_get(key.data, key.len, &v) && v.len == sizeof(xid))
		memcpy(&xid, v.data, sizeof(xid));
	pfree(key.data);
	pfree(v.data);
	return TransactionIdIsValid(xid) && !TransactionIdIsCurrentTransactionId(xid) &&
		XidInMVCCSnapshot(xid, GetActiveSnapshot());
}

/*
 * With the refs held, a snapshot that sees the head's commit: the active
 * one, or (read committed) a newer one, pushed (true; the caller pops it).
 * A transaction whose snapshot can't move fails instead, to be retried,
 * as a commit on top of a head it can't see would undo that head's rows.
 */
static bool
see_head(const char *name)
{
	if (!head_moved_unseen(name))
		return false;
	if (IsolationUsesXactSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access: branch \"%s\" moved since this transaction's snapshot",
						name),
				 errhint("The transaction might succeed if retried.")));
	PushActiveSnapshot(GetLatestSnapshot());
	return true;
}

/* A branch's baseline; false if it has none. */
static bool
branch_baseline(const char *name, ProllyNodeRef *baseline)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;
	ProllyHash	h;

	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_BASELINE);
	appendStringInfoString(&key, name);
	found = refs_get(key.data, key.len, &v) &&
		get_hash(v.data, v.data + v.len, &h) && h.len > 0;
	if (found)
		ref_of(&h, baseline);
	pfree(key.data);
	pfree(v.data);
	return found;
}

/* The working set's root branch name's baseline's fold made, if known. */
static bool
branch_baseline_root(const char *name, ProllyNodeRef *root)
{
	StringInfoData key;
	StringInfoData v;
	bool		found = false;
	ProllyHash	h;

	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_BASELINE);
	appendStringInfoString(&key, name);
	if (refs_get(key.data, key.len, &v))
	{
		const char *p = get_hash(v.data, v.data + v.len, &h);

		if (p < v.data + v.len)
		{
			get_hash(p, v.data + v.len, &h);
			found = h.len > 0;
			if (found)
				ref_of(&h, root);
		}
	}
	pfree(key.data);
	pfree(v.data);
	return found;
}

/*
 * Make baseline (NULL: none) branch name's baseline; 'root' (NULL: not
 * known) is the working set's root its fold made.
 */
static void
branch_set_baseline(const char *name, const ProllyNodeRef *baseline,
					const ProllyNodeRef *root)
{
	StringInfoData key;
	StringInfoData v;

	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_BASELINE);
	appendStringInfoString(&key, name);
	if (baseline)
		put_hash(&v, &baseline->hash);
	if (baseline && root)
		put_hash(&v, &root->hash);
	refs_put(key.data, key.len, baseline ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
}

typedef struct FindBranch
{
	const char *name;
	Oid			branch;
} FindBranch;

static void
find_branch(const char *name, Oid branch, void *arg)
{
	FindBranch *fb = arg;

	if (strcmp(name, fb->name) == 0)
		fb->branch = branch;
}

/* The id of the branch called name; InvalidOid if there's none. */
static Oid
branch_oid(const char *name)
{
	FindBranch	fb = {name, InvalidOid};

	branch_ops()->branches(find_branch, &fb);
	return fb.branch;
}

/* ------------------------------------------------------------------------
 * Commits
 * ------------------------------------------------------------------------
 */

static void
add_item(ProllyTreeBuilder *b, const char *key, const StringInfoData *value)
{
	ProllyKey	k = {(const uint8 *) key, strlen(key)};
	ProllyValue v = {(const uint8 *) value->data, value->len};

	PROLLY_CALL(tree, build_add, b, &k, &v);
}

/* Write a commit's tree; its id is the root's hash. */
static void
write_commit(const ProllyHash *root, const ProllyHash *parents, int nparents,
			 const char *author, const char *message, TimestampTz at,
			 ProllyHash *id)
{
	ProllyTreeBuilder *b = PROLLY_CALL(tree, build_begin, &env,
									   CurrentMemoryContext);
	StringInfoData v;
	ProllyNodeRef ref;

	/* Keys in order: author, message, parents, root, time. */
	initStringInfo(&v);
	appendStringInfoString(&v, author);
	add_item(b, "author", &v);
	resetStringInfo(&v);
	appendStringInfoString(&v, message);
	add_item(b, "message", &v);
	resetStringInfo(&v);
	for (int i = 0; i < nparents; i++)
		put_hash(&v, &parents[i]);
	add_item(b, "parents", &v);
	resetStringInfo(&v);
	put_hash(&v, root);
	add_item(b, "root", &v);
	resetStringInfo(&v);
	for (int i = 7; i >= 0; i--)
		appendStringInfoChar(&v, (char) ((uint64) at >> (i * 8)));
	add_item(b, "time", &v);
	PROLLY_CALL(tree, build_finish, b, &ref);
	*id = ref.hash;
	pfree(v.data);
}

static bool
read_item(const ProllyNodeRef *commit, const char *key, StringInfo out)
{
	ProllyKey	k = {(const uint8 *) key, strlen(key)};

	return PROLLY_CALL(tree, get, &env, commit, &k, out);
}

static bool
copies_commit_get(const ProllyHash *id, ProllyCommitInfo *info)
{
	ProllyNodeRef ref;
	StringInfoData v;
	ProllyHash	root;
	const char *p;
	const char *end;

	refs_init();
	if (id->len == 0 || !PROLLY_CALL(env.chunk_store, has, env.chunks, id))
		return false;
	ref_of(id, &ref);
	initStringInfo(&v);
	if (!read_item(&ref, "root", &v))
		return false;			/* a chunk, but not a commit */
	memset(info, 0, sizeof(*info));
	info->id = *id;
	(void) get_hash(v.data, v.data + v.len, &root);
	ref_of(&root, &info->root);
	resetStringInfo(&v);
	(void) read_item(&ref, "parents", &v);
	p = v.data;
	end = v.data + v.len;
	while (p < end && info->nparents < PROLLY_MAX_PARENTS)
		p = get_hash(p, end, &info->parents[info->nparents++]);
	resetStringInfo(&v);
	(void) read_item(&ref, "author", &v);
	info->author = pstrdup(v.data);
	resetStringInfo(&v);
	(void) read_item(&ref, "message", &v);
	info->message = pstrdup(v.data);
	resetStringInfo(&v);
	if (read_item(&ref, "time", &v) && v.len == 8)
	{
		uint64		t = 0;

		for (int i = 0; i < 8; i++)
			t = (t << 8) | (uint8) v.data[i];
		info->committed_at = (TimestampTz) t;
	}
	pfree(v.data);
	return true;
}

/* A commit id in hex; false if refname isn't one. */
static bool
parse_commit_id(const char *refname, ProllyHash *id)
{
	size_t		len = strlen(refname);

	if (len == 0 || len % 2 != 0 || len / 2 > PROLLY_HASH_MAX_LEN)
		return false;
	for (size_t i = 0; i < len; i++)
	{
		if (!isxdigit((unsigned char) refname[i]))
			return false;
	}
	memset(id, 0, sizeof(*id));
	id->len = len / 2;
	(void) hex_decode(refname, len, (char *) id->bytes);
	return true;
}

static bool
resolve_base(const char *refname, ProllyHash *commit_id)
{
	ProllyCommitInfo info;

	/* HEAD: the session branch's head commit, as in Dolt. */
	if (strcmp(refname, "HEAD") == 0)
		refname = branch_ops()->current();
	if (OidIsValid(branch_oid(refname)))
		return branch_head(refname, commit_id);
	return parse_commit_id(refname, commit_id) &&
		copies_commit_get(commit_id, &info);
}

static bool
copies_resolve(const char *refname, ProllyHash *commit_id)
{
	size_t		baselen = strcspn(refname, "~^");
	char	   *base = pnstrdup(refname, baselen);
	const char *p = refname + baselen;
	bool		found;

	refs_init();
	found = resolve_base(base, commit_id);
	pfree(base);

	/* Ancestry steps: ~n, the nth first parent; ^n, the nth parent. */
	while (found && *p != '\0')
	{
		char		op = *p++;
		long		n = 1;
		ProllyCommitInfo info;

		if (isdigit((unsigned char) *p))
		{
			char	   *end;

			n = strtol(p, &end, 10);
			p = end;
		}
		if (op == '~')
		{
			for (long i = 0; i < n && found; i++)
			{
				found = copies_commit_get(commit_id, &info) && info.nparents > 0;
				if (found)
					*commit_id = info.parents[0];
			}
		}
		else if (op == '^')
		{
			if (n == 0)
				continue;
			found = copies_commit_get(commit_id, &info) && n <= info.nparents;
			if (found)
				*commit_id = info.parents[n - 1];
		}
		else
			found = false;
	}
	return found;
}

static bool
copies_lookup_commit(const ProllyHash *id, ProllyCommitInfo *info, void *arg)
{
	return copies_commit_get(id, info);
}

/* The newest common ancestor of a and b, each commit visited once. */
static bool
copies_merge_base(const ProllyHash *a, const ProllyHash *b, ProllyHash *base)
{
	refs_init();
	return prolly_history_merge_base(a, b, copies_lookup_commit, NULL, base);
}

/*
 * The session branch's versioned schemas, folded from its baseline; with
 * next, a new baseline begun.
 */
static void
fold_from_baseline(ProllyNodeRef *root, ProllyNodeRef *next)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef baseline;
	bool		have = branch_baseline(branch_ops()->current(), &baseline);

	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, fold, have ? &baseline : NULL, root, next);
}

static void
fold(ProllyNodeRef *root)
{
	fold_from_baseline(root, NULL);
}

/*
 * Hold the refs for a commit to branch name with these parents.  Parents
 * read before the refs were held (a merge's, say) may no longer have the
 * head among them: committing would leave the commit another made
 * meanwhile off the branch.  True if a snapshot that sees the head was
 * pushed, for the commit's fold (see_head()).
 */
static bool
hold_for_commit(const char *name, const ProllyHash *parents, int nparents)
{
	ProllyHash	head;

	lock_refs();
	if (nparents > 0 && branch_head(name, &head) && !same_hash(&head, &parents[0]))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access: branch \"%s\" moved while this was being committed",
						name),
				 errhint("The transaction might succeed if retried.")));
	return see_head(name);
}

/*
 * Commit the session branch as it is, with these parents; the fold is the
 * branch's next baseline.
 */
static void
commit_branch(const char *message, const ProllyHash *parents, int nparents,
			  ProllyHash *commit_id)
{
	char	   *name = branch_ops()->current();
	ProllyNodeRef root;
	ProllyNodeRef next;
	bool		pushed;

	pushed = hold_for_commit(name, parents, nparents);
	fold_from_baseline(&root, &next);
	if (pushed)
		PopActiveSnapshot();
	write_commit(&root.hash, parents, nparents, prolly_commit_author(),
				 message, GetCurrentTransactionStartTimestamp(), commit_id);
	branch_set_head(name, commit_id);
	branch_set_baseline(name, &next, &root);
}

/* ------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------
 */

/* Move the session's branch to a commit's root: its rows written. */
static void
move_to(void *arg)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	PROLLY_CALL(dc.diff_merge, move_to, (const ProllyNodeRef *) arg);
}

static void
copies_branch_create(const char *name, const char *start_point)
{
	const char *from = start_point ? start_point : branch_ops()->current();
	ProllyHash	head;
	ProllyHash	id;
	ProllyCommitInfo info;

	lock_refs();
	if (OidIsValid(branch_oid(from)))
	{
		ProllyNodeRef baseline;

		branch_ops()->create(name, from);
		/* (a branch of the same name deleted before left no entry) */
		branch_set_head(name, branch_head(from, &head) ? &head : NULL);
		/* its copies have the same changes since the same baseline */
		branch_set_baseline(name, branch_baseline(from, &baseline) ? &baseline : NULL,
							NULL);
		return;
	}

	/* A commit: a copy of the session's branch, moved there. */
	if (!copies_resolve(from, &id) || !copies_commit_get(&id, &info))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch or commit \"%s\" does not exist", from)));
	branch_ops()->create(name, NULL);
	{
		ProllyNodeRef baseline;

		branch_set_baseline(name,
							branch_baseline(branch_ops()->current(), &baseline) ?
							&baseline : NULL, NULL);
	}
	branch_ops()->run_on(name, move_to, &info.root);
	branch_set_head(name, &id);
}

static void
copies_branch_delete(const char *name, bool force)
{
	ProllyHash	head;
	ProllyHash	ours;
	ProllyHash	base;

	lock_refs();
	if (!force && OidIsValid(branch_oid(name)) && branch_head(name, &head) &&
		!(branch_head(branch_ops()->current(), &ours) &&
		  copies_merge_base(&head, &ours, &base) && same_hash(&base, &head)))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" is not fully merged", name),
				 errhint("Delete it anyway with -D.")));
	branch_ops()->drop(name);
	branch_set_head(name, NULL);
	branch_set_baseline(name, NULL, NULL);

	/* and the merge in progress on it, which a branch made later isn't in */
	{
		StringInfoData key;
		ProllyMergeState ms;

		initStringInfo(&key);
		merge_key(&key, name);
		if (merge_state_of(name, &ms))
		{
			refs_put(key.data, key.len, NULL);
			if (ms.pick)
			{
				resetStringInfo(&key);
				appendStringInfoChar(&key, TAG_PICK);
				appendStringInfoString(&key, name);
				refs_put(key.data, key.len, NULL);
			}
		}
		pfree(key.data);
	}
}

static bool
copies_branch_lookup(const char *name, ProllyBranchId *branch)
{
	Oid			oid = branch_oid(name);

	*branch = (ProllyBranchId) oid;
	return OidIsValid(oid);
}

/*
 * A working-set checkpoint: the session branch folded as a commit would
 * be, its fold the branch's baseline.  As a commit, it folds by the latest
 * snapshot if the branch moved since the transaction's (an error if the
 * transaction keeps one).
 */
static void
copies_checkpoint(ProllyNodeRef *root)
{
	char	   *name = branch_ops()->current();
	ProllyNodeRef next;
	bool		pushed;

	lock_refs();
	pushed = see_head(name);
	fold_from_baseline(root, &next);
	if (pushed)
		PopActiveSnapshot();
	branch_set_baseline(name, &next, root);
}

/*
 * Leaving a branch with changes takes a working-set checkpoint of it, so
 * the pages it changed needn't be kept until its next commit: if its
 * working set isn't its head's, nor what its baseline's fold made already.
 * Not in a transaction that can't write, nor in a transaction block (the
 * refs, held to the transaction's end, would hold up others' commits), nor
 * if the branch moved unseen by the snapshot (its fold wouldn't be the
 * branch's).  (A session choosing its first branch, as it connects, leaves
 * none.)
 */
static void
copies_checkout(const char *name)
{
	char	   *current = branch_ops()->current();

	if (strcmp(current, name) != 0 && ActiveSnapshotSet() &&
		!XactReadOnly && !RecoveryInProgress() && !IsTransactionBlock() &&
		OidIsValid(branch_oid(name)))
	{
		ProllyNodeRef working;
		ProllyNodeRef kept;
		ProllyNodeRef next;
		ProllyHash	head;

		fold(&working);
		if (!(branch_head(current, &head) &&
			  copies_commit_working_set(&head, &kept) &&
			  same_hash(&kept.hash, &working.hash)) &&
			!(branch_baseline_root(current, &kept) &&
			  same_hash(&kept.hash, &working.hash)))
		{
			lock_refs();
			if (!head_moved_unseen(current))
			{
				fold_from_baseline(&working, &next);
				branch_set_baseline(current, &next, &working);
			}
		}
	}
	branch_ops()->checkout(name);
}

/*
 * Commit the session's branch; ending a merge in progress, once its
 * conflicts are resolved, with both heads as parents.
 */
static void
copies_commit(const char *message, ProllyHash *commit_id)
{
	ProllyHash	parents[2];
	int			nparents = 0;
	ProllyMergeState ms;

	lock_refs();
	if (branch_head(branch_ops()->current(), &parents[0]))
		nparents++;
	if (copies_merge_state(&ms))
	{
		if (ms.ntables > 0)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot commit with unresolved merge conflicts"),
					 errdetail("Table \"%s\" has conflicts.",
							   get_rel_name(ms.tables[0])),
					 errhint("Resolve them with dolt_conflicts_resolve(), or abort the merge.")));
		if (!ms.pick)
			parents[nparents++] = ms.theirs;
		copies_set_merge_state(NULL);
	}
	commit_branch(message, parents, nparents, commit_id);
}

/*
 * The working set: the branch as it is (the transaction sees it), or, if
 * given, a root to commit instead; the branch's fold is its next baseline
 * either way.
 */
static void
copies_commit_with(const char *message, const ProllyHash *parents,
				   int nparents, const ProllyNodeRef *working_set,
				   ProllyHash *commit_id)
{
	ProllyNodeRef root;
	ProllyNodeRef next;
	char	   *name;
	bool		pushed;

	if (working_set == NULL)
	{
		commit_branch(message, parents, nparents, commit_id);
		return;
	}
	name = branch_ops()->current();
	pushed = hold_for_commit(name, parents, nparents);
	fold_from_baseline(&root, &next);
	if (pushed)
		PopActiveSnapshot();
	write_commit(&working_set->hash, parents, nparents, prolly_commit_author(),
				 message, GetCurrentTransactionStartTimestamp(), commit_id);
	branch_set_head(name, commit_id);
	branch_set_baseline(name, &next, &root);
}

static void
copies_set_head(const ProllyHash *commit_id)
{
	lock_refs();
	branch_set_head(branch_ops()->current(), commit_id);
}

/*
 * What a commit recorded, to compare with working_set(): its root, the
 * same canonical form.
 */
static bool
copies_commit_working_set(const ProllyHash *commit_id, ProllyNodeRef *root)
{
	ProllyCommitInfo info;

	if (!copies_commit_get(commit_id, &info))
		return false;
	*root = info.root;
	return true;
}

typedef struct VisitBranches
{
	ProllyBranchVisit visit;
	void	   *arg;
} VisitBranches;

static void
visit_branch(const char *name, Oid branch, void *arg)
{
	VisitBranches *vb = arg;
	ProllyHash	head;

	(void) branch_head(name, &head);
	vb->visit(name, &head, vb->arg);
}

static void
copies_branches(ProllyBranchVisit visit, void *arg)
{
	VisitBranches vb = {visit, arg};

	refs_init();
	branch_ops()->branches(visit_branch, &vb);
}

/* A branch's versioned schemas as they are, folded: its working set. */
static void
fold_into(void *arg)
{
	fold((ProllyNodeRef *) arg);
}

static bool
copies_working_set(const char *branch, ProllyNodeRef *root)
{
	if (branch == NULL)
	{
		fold(root);
		return true;
	}
	if (!OidIsValid(branch_oid(branch)))
		return false;
	branch_ops()->run_on(branch, fold_into, root);
	return true;
}

/*
 * Make the commit ref names the session branch's head, and move the branch
 * there: its rows written as the commit has them.
 */
static void
copies_reset(const char *ref)
{
	ProllyHash	target;
	ProllyCommitInfo info;
	bool		pushed;

	lock_refs();
	if (!copies_resolve(ref, &target) || !copies_commit_get(&target, &info))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	/* (the rows moved as a snapshot that sees the head has them) */
	pushed = see_head(branch_ops()->current());
	move_to(&info.root);
	if (pushed)
		PopActiveSnapshot();
	branch_set_head(branch_ops()->current(), &target);
	copies_set_merge_state(NULL);	/* a reset ends a merge */
}

static void
put_be32(StringInfo out, uint32 v)
{
	for (int i = 3; i >= 0; i--)
		appendStringInfoChar(out, (char) (v >> (i * 8)));
}

static uint32
get_be32(const char *p)
{
	const uint8 *d = (const uint8 *) p;

	return ((uint32) d[0] << 24) | ((uint32) d[1] << 16) |
		((uint32) d[2] << 8) | (uint32) d[3];
}

static void
merge_key(StringInfo key, const char *branch)
{
	appendStringInfoChar(key, TAG_MERGE);
	appendStringInfoString(key, branch);
}

/* The merge in progress on branch, if there is one. */
static bool
merge_state_of(const char *branch, ProllyMergeState *state)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;

	refs_init();
	initStringInfo(&key);
	initStringInfo(&v);
	merge_key(&key, branch);
	memset(state, 0, sizeof(*state));
	found = refs_get(key.data, key.len, &v);
	if (found)
	{
		const char *p = v.data;
		const char *end = v.data + v.len;

		p = get_hash(p, end, &state->theirs);
		p = get_hash(p, end, &state->base);
		state->ntables = (end - p) / 4;
		state->tables = palloc(sizeof(Oid) * Max(state->ntables, 1));
		for (int i = 0; i < state->ntables; i++)
			state->tables[i] = get_be32(p + i * 4);

		/* a cherry-pick's */
		resetStringInfo(&key);
		resetStringInfo(&v);
		appendStringInfoChar(&key, TAG_PICK);
		appendStringInfoString(&key, branch);
		state->pick = refs_get(key.data, key.len, &v);
		state->message = state->pick ? pnstrdup(v.data, v.len) : NULL;
	}
	pfree(key.data);
	pfree(v.data);
	return found;
}

static bool
copies_merge_state(ProllyMergeState *state)
{
	return merge_state_of(branch_ops()->current(), state);
}

/* (NULL: none) */
static void
copies_set_merge_state(const ProllyMergeState *state)
{
	StringInfoData key;
	StringInfoData v;
	ProllyMergeState cur;
	char	   *branch = branch_ops()->current();

	StringInfoData pkey;
	bool		had;

	refs_init();
	initStringInfo(&key);
	merge_key(&key, branch);
	initStringInfo(&pkey);
	appendStringInfoChar(&pkey, TAG_PICK);
	appendStringInfoString(&pkey, branch);
	had = merge_state_of(branch, &cur);
	if (state == NULL)
	{
		if (had)
			refs_put(key.data, key.len, NULL);
		if (had && cur.pick)
			refs_put(pkey.data, pkey.len, NULL);
	}
	else
	{
		initStringInfo(&v);
		put_hash(&v, &state->theirs);
		put_hash(&v, &state->base);
		for (int i = 0; i < state->ntables; i++)
			put_be32(&v, state->tables[i]);
		refs_put(key.data, key.len, &v);
		resetStringInfo(&v);
		if (state->pick)
		{
			appendStringInfoString(&v, state->message ? state->message : "");
			refs_put(pkey.data, pkey.len, &v);
		}
		else if (had && cur.pick)
			refs_put(pkey.data, pkey.len, NULL);
		pfree(v.data);
		if (!name_listed(merge_changed, branch))
		{
			MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

			merge_changed = lappend(merge_changed, pstrdup(branch));
			MemoryContextSwitchTo(old);
		}
	}
	pfree(key.data);
	pfree(pkey.data);
}

typedef struct WalkState
{
	ProllyTreeVisitNode visit;
	void	   *arg;
	List	   *seen;			/* commit ids walked */
} WalkState;

static void
walk_commit(WalkState *ws, const ProllyHash *id)
{
	ProllyCommitInfo info;
	ProllyNodeRef ref;
	ProllyDatabaseComponents dc;
	ListCell   *lc;
	ProllyHash *copy;

	foreach(lc, ws->seen)
	{
		if (same_hash(lfirst(lc), id))
			return;
	}
	copy = palloc(sizeof(ProllyHash));
	*copy = *id;
	ws->seen = lappend(ws->seen, copy);
	if (!copies_commit_get(id, &info))
		return;
	ref_of(id, &ref);
	PROLLY_CALL(tree, walk, &env, &ref, ws->visit, ws->arg);
	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, walk, &info.root, ws->visit, ws->arg);
	for (int i = 0; i < info.nparents; i++)
		walk_commit(ws, &info.parents[i]);
}

/*
 * The refs tree, every commit reachable from a head or a merge in
 * progress, with its root, and every baseline.
 */
static void
copies_walk(const ProllyNodeRef *refs, ProllyTreeVisitNode visit, void *arg)
{
	WalkState	ws = {visit, arg, NIL};
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	List	   *heads = NIL;
	List	   *baselines = NIL;
	ListCell   *lc;
	ProllyDatabaseComponents dc;

	refs_init();
	PROLLY_CALL(tree, walk, &env, refs, visit, arg);
	c = PROLLY_CALL(tree, cursor_open, &env, refs, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		const char *p = (const char *) v.data;
		const char *end = p + v.len;
		ProllyHash *h;

		if (k.len < 1 || (k.data[0] != TAG_BRANCH && k.data[0] != TAG_MERGE &&
						  k.data[0] != TAG_BASELINE))
			continue;
		h = palloc(sizeof(ProllyHash));
		p = get_hash(p, end, h);
		if (k.data[0] == TAG_BASELINE)
			baselines = lappend(baselines, h);
		else
			heads = lappend(heads, h);

		/* a merge in progress: theirs, then the base, both kept */
		if (k.data[0] == TAG_MERGE)
		{
			h = palloc(sizeof(ProllyHash));
			(void) get_hash(p, end, h);
			heads = lappend(heads, h);
		}
	}
	PROLLY_CALL(tree, cursor_close, c);
	foreach(lc, heads)
		walk_commit(&ws, lfirst(lc));
	prolly_database_components(&dc);
	foreach(lc, baselines)
	{
		ProllyNodeRef ref;

		ref_of(lfirst(lc), &ref);
		PROLLY_CALL(dc.canonicalizer, walk_baseline, &ref, visit, arg);
	}
}

static bool
copies_hold(void)
{
	lock_refs();
	return see_head(branch_ops()->current());
}

const ProllyRefsRoutine prolly_refs_copies = {
	.info = {
		.iface = PROLLY_IFACE_REFS,
		.impl_id = 2,
		.format_version = 1,
		.name = "copies",
		.description = "head commits of branches made by copying, and the "
		"commits, in the chunk store",
	},
	.branch_create = copies_branch_create,
	.branch_delete = copies_branch_delete,
	.branch_lookup = copies_branch_lookup,
	.checkout = copies_checkout,
	.commit = copies_commit,
	.resolve = copies_resolve,
	.commit_get = copies_commit_get,
	.merge_base = copies_merge_base,
	.commit_working_set = copies_commit_working_set,
	.commit_with = copies_commit_with,
	.branches = copies_branches,
	.working_set = copies_working_set,
	.reset = copies_reset,
	.merge_state = copies_merge_state,
	.set_merge_state = copies_set_merge_state,
	.hold = copies_hold,
	.walk = copies_walk,
	.checkpoint = copies_checkpoint,
	.set_head = copies_set_head,
};
