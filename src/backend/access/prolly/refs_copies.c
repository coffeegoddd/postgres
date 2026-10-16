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
 *
 * A branch without an entry has no commits yet.  A commit is a tree of its
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
#include "catalog/branch_ops.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#define TAG_BRANCH		'B'

static bool ready = false;
static const ProllyTreeRoutine *tree;
static ProllyTreeEnv env;

static int
memcmp_keys(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	if (r != 0)
		return r;
	return (a->len > b->len) - (a->len < b->len);
}

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
	env.compare = memcmp_keys;
	ready = true;
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

/* Make head (NULL: none) branch name's head commit. */
static void
branch_set_head(const char *name, const ProllyHash *head)
{
	StringInfoData key;
	StringInfoData v;

	initStringInfo(&key);
	initStringInfo(&v);
	branch_key(name, &key);
	if (head)
		put_hash(&v, head);
	refs_put(key.data, key.len, head ? &v : NULL);
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

/* The newest common ancestor of a and b: breadth first from both. */
static bool
copies_merge_base(const ProllyHash *a, const ProllyHash *b, ProllyHash *base)
{
	List	   *seen = NIL;
	List	   *queue = NIL;
	ProllyHash *h;

	refs_init();
	h = palloc(sizeof(ProllyHash));
	*h = *a;
	queue = lappend(queue, h);
	while (queue != NIL)
	{
		ProllyCommitInfo info;

		h = linitial(queue);
		queue = list_delete_first(queue);
		seen = lappend(seen, h);
		if (!copies_commit_get(h, &info))
			continue;
		for (int i = 0; i < info.nparents; i++)
		{
			ProllyHash *p = palloc(sizeof(ProllyHash));

			*p = info.parents[i];
			queue = lappend(queue, p);
		}
	}
	h = palloc(sizeof(ProllyHash));
	*h = *b;
	queue = lappend(queue, h);
	while (queue != NIL)
	{
		ProllyCommitInfo info;
		ListCell   *lc;

		h = linitial(queue);
		queue = list_delete_first(queue);
		foreach(lc, seen)
		{
			if (same_hash(lfirst(lc), h))
			{
				*base = *h;
				return true;
			}
		}
		if (!copies_commit_get(h, &info))
			continue;
		for (int i = 0; i < info.nparents; i++)
		{
			ProllyHash *p = palloc(sizeof(ProllyHash));

			*p = info.parents[i];
			queue = lappend(queue, p);
		}
	}
	return false;
}

/* The session branch's versioned schemas, folded. */
static void
fold(ProllyNodeRef *root)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, fold, root);
}

/* Commit the session branch as it is, with these parents. */
static void
commit_branch(const char *message, const ProllyHash *parents, int nparents,
			  ProllyHash *commit_id)
{
	char	   *name = branch_ops()->current();
	ProllyNodeRef root;

	lock_refs();
	fold(&root);
	write_commit(&root.hash, parents, nparents, prolly_commit_author(),
				 message, GetCurrentTransactionStartTimestamp(), commit_id);
	branch_set_head(name, commit_id);
}

/* ------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------
 */

static void
copies_branch_create(const char *name, const char *start_point)
{
	const char *from = start_point ? start_point : branch_ops()->current();
	ProllyHash	head;

	lock_refs();
	if (!OidIsValid(branch_oid(from)))
	{
		ProllyHash	id;

		if (copies_resolve(from, &id))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a branch can't start at a commit yet"),
					 errhint("Start it at a branch.")));
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", from)));
	}
	branch_ops()->create(name, from);
	/* (a branch of the same name deleted before left no entry) */
	branch_set_head(name, branch_head(from, &head) ? &head : NULL);
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
}

static bool
copies_branch_lookup(const char *name, ProllyBranchId *branch)
{
	Oid			oid = branch_oid(name);

	*branch = (ProllyBranchId) oid;
	return OidIsValid(oid);
}

static void
copies_checkout(const char *name)
{
	branch_ops()->checkout(name);
}

static void
copies_commit(const char *message, ProllyHash *commit_id)
{
	ProllyHash	head;

	lock_refs();
	if (branch_head(branch_ops()->current(), &head))
		commit_branch(message, &head, 1, commit_id);
	else
		commit_branch(message, NULL, 0, commit_id);
}

/* The working set: the branch as it is (the transaction sees it). */
static void
copies_commit_with(const char *message, const ProllyHash *parents,
				   int nparents, const ProllyNodeRef *working_set,
				   ProllyHash *commit_id)
{
	commit_branch(message, parents, nparents, commit_id);
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

/* No merges yet: none is in progress. */
static bool
copies_merge_state(ProllyMergeState *state)
{
	return false;
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

/* The refs tree, and every commit reachable from a head, with its root. */
static void
copies_walk(const ProllyNodeRef *refs, ProllyTreeVisitNode visit, void *arg)
{
	WalkState	ws = {visit, arg, NIL};
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	List	   *heads = NIL;
	ListCell   *lc;

	refs_init();
	PROLLY_CALL(tree, walk, &env, refs, visit, arg);
	c = PROLLY_CALL(tree, cursor_open, &env, refs, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		ProllyHash *h;

		if (k.len < 1 || k.data[0] != TAG_BRANCH)
			continue;
		h = palloc(sizeof(ProllyHash));
		(void) get_hash((const char *) v.data, (const char *) v.data + v.len, h);
		heads = lappend(heads, h);
	}
	PROLLY_CALL(tree, cursor_close, c);
	foreach(lc, heads)
		walk_commit(&ws, lfirst(lc));
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
	.merge_state = copies_merge_state,
	.walk = copies_walk,
};
