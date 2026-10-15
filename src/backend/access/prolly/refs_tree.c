/*-------------------------------------------------------------------------
 *
 * refs_tree.c
 *	  Refs implementation "tree": a database's branches and commits in a
 *	  prolly tree, kept as the head of a reserved branch of the commit
 *	  pipeline (PROLLY_REFS_BRANCH).
 *
 * So refs are transactional, WAL-logged, crash-safe and replicated exactly
 * as working sets are, with no store of their own.  The refs tree's keys
 * sort by memcmp:
 *
 *	'B' name		-> branch id (4 bytes, big-endian), head commit
 *					   (hash length, hash; length 0: no commit yet)
 *	'C' commit id	-> the local root (hash length, hash) the commit
 *					   recorded: the working set to start a branch from
 *	'N'				-> the next branch id to give out (4 bytes)
 *
 * A commit is a tree of its own (keys by memcmp), and its id is that
 * tree's root hash, so the same commit content has the same id in every
 * database:
 *
 *	"author"	-> the author
 *	"message"	-> the message
 *	"parents"	-> the parents' ids, each a length byte and the hash
 *	"root"		-> the commit's canonical root hash (length byte, hash),
 *				   which the canonicalizer derives from the working set
 *	"time"		-> microseconds since the Postgres epoch, 8 bytes big-endian
 *
 * The branch "main" (PROLLY_MAIN_BRANCH) exists from the start, without an
 * entry until something is committed to it.
 *
 * Changes are kept in the transaction's memory and written at pre-commit,
 * under the refs branch's lock, which a transaction takes before it reads
 * the refs it changes (so writers of refs take turns, and ids are given
 * out once).  New branches' working sets are enqueued then too, under
 * their own branch numbers.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/refs_tree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "access/prolly_xact.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#define TAG_BRANCH		'B'
#define TAG_COMMIT		'C'
#define TAG_NEXT		'N'

#define MAX_BRANCH_NAME	255

/* A pending change to the refs tree; value NULL deletes. */
typedef struct RefEdit
{
	StringInfoData key;
	StringInfoData value;
	bool		is_delete;
} RefEdit;

/* A new or reset branch's working set, to enqueue at commit. */
typedef struct NewHead
{
	ProllyBranchId branch;
	ProllyHash	root;
} NewHead;

typedef struct RefsXact
{
	MemoryContext mcxt;
	bool		locked;			/* holds the refs branch's lock */
	List	   *edits;			/* RefEdit *, in the order made */
	List	   *heads;			/* NewHead * */
	ProllyCommitTicket tickets[64];
	int			ntickets;
} RefsXact;

static RefsXact *rx = NULL;
static bool callback_registered = false;

/* Components, and an environment for trees keyed by memcmp. */
static bool ready = false;
static ProllyComponents comps;
static ProllyTreeEnv env;
static ProllyRootEnv root_env;

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
	prolly_components_for_engine(&prolly_engine_d1.info, &comps);
	memset(&env, 0, sizeof(env));
	env.node_format = comps.node_format;
	env.chunker = comps.chunker;
	env.hash = comps.hash;
	env.node_store = comps.node_store;
	env.store = PROLLY_CALL(comps.node_store, open, NULL);
	env.log = comps.log;
	env.compare = memcmp_keys;
	root_env.tree = comps.tree;
	root_env.tree_env = &env;
	ready = true;
}

static void
empty_tree(ProllyNodeRef *root)
{
	ProllyTreeBuilder *b = PROLLY_CALL(comps.tree, build_begin, &env,
									   CurrentMemoryContext);

	PROLLY_CALL(comps.tree, build_finish, b, root);
}

static void put_be32(StringInfo out, uint32 v)
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

/* ------------------------------------------------------------------------
 * The transaction's changes
 * ------------------------------------------------------------------------
 */

static void refs_xact_callback(XactEvent event, void *arg);

static void
xact_begin(void)
{
	refs_init();
	if (rx != NULL)
		return;
	rx = MemoryContextAllocZero(TopTransactionContext, sizeof(RefsXact));
	rx->mcxt = AllocSetContextCreate(TopTransactionContext, "prolly refs",
									 ALLOCSET_SMALL_SIZES);
	if (!callback_registered)
	{
		RegisterXactCallback(refs_xact_callback, NULL);
		callback_registered = true;
	}
}

/* Writers of refs take turns: the lock is held until the transaction ends. */
static void
lock_refs(void)
{
	xact_begin();
	if (!rx->locked)
	{
		PROLLY_CALL(comps.commit, branch_lock, PROLLY_REFS_BRANCH);
		rx->locked = true;
	}
}

static void
add_edit(const char *key, int keylen, const StringInfoData *value)
{
	MemoryContext old = MemoryContextSwitchTo(rx->mcxt);
	RefEdit    *e = palloc0(sizeof(RefEdit));

	initStringInfo(&e->key);
	appendBinaryStringInfo(&e->key, key, keylen);
	initStringInfo(&e->value);
	if (value)
		appendBinaryStringInfo(&e->value, value->data, value->len);
	e->is_delete = (value == NULL);
	rx->edits = lappend(rx->edits, e);
	MemoryContextSwitchTo(old);
}

/* The newest refs root, as tip() gives it (the transaction holds the lock). */
static bool
refs_root(ProllyNodeRef *root)
{
	refs_init();
	if (rx && rx->locked)
		return PROLLY_CALL(comps.commit, tip, PROLLY_REFS_BRANCH, root);
	return PROLLY_CALL(comps.commit, head, PROLLY_REFS_BRANCH, root);
}

/* Look key up in the refs, with the transaction's changes over them. */
static bool
refs_get(const char *key, int keylen, StringInfo value)
{
	ProllyNodeRef root;
	ProllyKey	k = {(const uint8 *) key, keylen};

	if (rx)
	{
		ListCell   *lc;
		RefEdit    *found = NULL;

		foreach(lc, rx->edits)
		{
			RefEdit    *e = lfirst(lc);

			if (e->key.len == keylen && memcmp(e->key.data, key, keylen) == 0)
				found = e;
		}
		if (found)
		{
			if (found->is_delete)
				return false;
			appendBinaryStringInfo(value, found->value.data, found->value.len);
			return true;
		}
	}
	if (!refs_root(&root))
		return false;
	return PROLLY_CALL(comps.tree, get, &env, &root, &k, value);
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

/* A branch's id and head commit (len 0: none); false if there's no branch. */
static bool
branch_get(const char *name, ProllyBranchId *id, ProllyHash *head)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;

	initStringInfo(&key);
	initStringInfo(&v);
	branch_key(name, &key);
	found = refs_get(key.data, key.len, &v);
	if (found)
	{
		if (v.len < 5)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly branch \"%s\" has an invalid entry", name)));
		*id = get_be32(v.data);
		(void) get_hash(v.data + 4, v.data + v.len, head);
	}
	else if (strcmp(name, "main") == 0)
	{
		*id = PROLLY_MAIN_BRANCH;
		memset(head, 0, sizeof(*head));
		found = true;
	}
	pfree(key.data);
	pfree(v.data);
	return found;
}

static void
branch_put(const char *name, ProllyBranchId id, const ProllyHash *head)
{
	StringInfoData key;
	StringInfoData v;

	initStringInfo(&key);
	initStringInfo(&v);
	branch_key(name, &key);
	put_be32(&v, id);
	put_hash(&v, head);
	add_edit(key.data, key.len, &v);
	pfree(key.data);
	pfree(v.data);
}

/* The name of branch id: scan the branch entries. */
static char *
branch_name(ProllyBranchId id)
{
	ProllyNodeRef root;
	char	   *name = NULL;
	ListCell   *lc;

	if (rx)
	{
		foreach(lc, rx->edits)
		{
			RefEdit    *e = lfirst(lc);

			if (!e->is_delete && e->key.data[0] == TAG_BRANCH &&
				e->value.len >= 4 && get_be32(e->value.data) == id)
				name = pnstrdup(e->key.data + 1, e->key.len - 1);
		}
		if (name)
			return name;
	}
	if (refs_root(&root))
	{
		ProllyTreeCursor *c = PROLLY_CALL(comps.tree, cursor_open, &env, &root,
										  CurrentMemoryContext);
		uint8		tag = TAG_BRANCH;
		ProllyKey	from = {&tag, 1};
		ProllyKey	k;
		ProllyValue v;

		PROLLY_CALL(comps.tree, cursor_seek, c, &from, ForwardScanDirection);
		while (PROLLY_CALL(comps.tree, cursor_next, c, &k, &v))
		{
			if (k.len < 1 || k.data[0] != TAG_BRANCH)
				break;
			if (v.len >= 4 && get_be32((const char *) v.data) == id)
			{
				name = pnstrdup((const char *) k.data + 1, k.len - 1);
				break;
			}
		}
		PROLLY_CALL(comps.tree, cursor_close, c);
	}
	if (name == NULL && id == PROLLY_MAIN_BRANCH)
		name = pstrdup("main");
	if (name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the session's prolly branch no longer exists")));
	return name;
}

/*
 * Names can't be empty, start with '-' (an option to the dolt_ functions),
 * or hold '~' or '^' (ancestry steps in refs) or whitespace.
 */
static void
check_branch_name(const char *name)
{
	if (name[0] == '\0' || name[0] == '-' || strlen(name) > MAX_BRANCH_NAME ||
		strpbrk(name, "~^ \t\n\r") != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid prolly branch name \"%s\"", name)));
}

/* A branch's working set (its head in the commit pipeline). */
static bool
working_root(ProllyBranchId id, ProllyNodeRef *root)
{
	return PROLLY_CALL(comps.commit, head, id, root);
}

/* ------------------------------------------------------------------------
 * Commits
 * ------------------------------------------------------------------------
 */

static void
commit_key(const ProllyHash *id, StringInfo key)
{
	appendStringInfoChar(key, TAG_COMMIT);
	appendBinaryStringInfo(key, id->bytes, id->len);
}

/* The local root commit id recorded; false if the refs don't know it. */
static bool
commit_local_root(const ProllyHash *id, ProllyHash *root)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;

	initStringInfo(&key);
	initStringInfo(&v);
	commit_key(id, &key);
	found = refs_get(key.data, key.len, &v);
	if (found)
		(void) get_hash(v.data, v.data + v.len, root);
	pfree(key.data);
	pfree(v.data);
	return found;
}

static void
add_item(ProllyTreeBuilder *b, const char *key, const StringInfoData *value)
{
	ProllyKey	k = {(const uint8 *) key, strlen(key)};
	ProllyValue v = {(const uint8 *) value->data, value->len};

	PROLLY_CALL(comps.tree, build_add, b, &k, &v);
}

/* Write a commit's tree; its id is the root's hash. */
static void
write_commit(const ProllyHash *root, const ProllyHash *parents, int nparents,
			 const char *author, const char *message, TimestampTz at,
			 ProllyHash *id)
{
	ProllyTreeBuilder *b = PROLLY_CALL(comps.tree, build_begin, &env,
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
	PROLLY_CALL(comps.tree, build_finish, b, &ref);
	*id = ref.hash;
	pfree(v.data);
}

static bool
read_item(const ProllyNodeRef *commit, const char *key, StringInfo out)
{
	ProllyKey	k = {(const uint8 *) key, strlen(key)};

	return PROLLY_CALL(comps.tree, get, &env, commit, &k, out);
}

static bool
tree_commit_get(const ProllyHash *id, ProllyCommitInfo *info)
{
	ProllyNodeRef ref;
	StringInfoData v;
	ProllyHash	root;
	const char *p;
	const char *end;

	refs_init();
	if (!commit_local_root(id, &root))
		return false;
	ref_of(id, &ref);
	memset(info, 0, sizeof(*info));
	info->id = *id;
	initStringInfo(&v);
	if (!read_item(&ref, "root", &v))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit has no root")));
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

/* A 64-digit hex commit id; false if refname isn't one. */
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
	ProllyBranchId id;
	ProllyHash	head;
	ProllyHash	root;

	/* HEAD: the session branch's head commit, as in Dolt. */
	if (strcmp(refname, "HEAD") == 0)
		refname = branch_name(prolly_session_branch());
	if (branch_get(refname, &id, &head))
	{
		if (head.len == 0)
			return false;
		*commit_id = head;
		return true;
	}
	if (parse_commit_id(refname, commit_id) &&
		commit_local_root(commit_id, &root))
		return true;
	return false;
}

static bool
tree_resolve(const char *refname, ProllyHash *commit_id)
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
				found = tree_commit_get(commit_id, &info) && info.nparents > 0;
				if (found)
					*commit_id = info.parents[0];
			}
		}
		else if (op == '^')
		{
			if (n == 0)
				continue;
			found = tree_commit_get(commit_id, &info) && n <= info.nparents;
			if (found)
				*commit_id = info.parents[n - 1];
		}
		else
			found = false;
	}
	return found;
}

/* ------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------
 */

static bool
tree_branch_lookup(const char *name, ProllyBranchId *branch)
{
	ProllyHash	head;

	refs_init();
	return branch_get(name, branch, &head);
}

static void
tree_branch_create(const char *name, const char *start_point)
{
	ProllyBranchId id;
	ProllyHash	head;
	ProllyHash	start_commit = {0};
	ProllyHash	root;
	StringInfoData v;
	ProllyBranchId next;
	NewHead    *nh;
	MemoryContext old;

	check_branch_name(name);
	lock_refs();
	if (branch_get(name, &id, &head))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("prolly branch \"%s\" already exists", name)));

	/* Where it starts: a commit's working set, or the current branch's. */
	if (start_point)
	{
		if (!tree_resolve(start_point, &start_commit))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("prolly ref \"%s\" does not exist", start_point)));
	}
	else
	{
		char	   *cur = branch_name(prolly_session_branch());

		if (!branch_get(cur, &id, &start_commit))
			memset(&start_commit, 0, sizeof(start_commit));
	}
	if (start_commit.len > 0)
	{
		if (!commit_local_root(&start_commit, &root))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly commit has no working set")));
	}
	else
	{
		ProllyNodeRef wr;

		if (working_root(prolly_session_branch(), &wr))
			root = wr.hash;
		else
		{
			PROLLY_CALL(comps.root_layout, empty, &root_env, &wr);
			root = wr.hash;
		}
	}

	initStringInfo(&v);
	next = refs_get((char[]) {TAG_NEXT}, 1, &v) && v.len == 4 ?
		get_be32(v.data) : PROLLY_MAIN_BRANCH + 1;
	if (next >= PROLLY_REFS_BRANCH)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many prolly branches")));
	resetStringInfo(&v);
	put_be32(&v, next + 1);
	add_edit((char[]) {TAG_NEXT}, 1, &v);
	branch_put(name, next, &start_commit);
	old = MemoryContextSwitchTo(rx->mcxt);
	nh = palloc(sizeof(NewHead));
	nh->branch = next;
	nh->root = root;
	rx->heads = lappend(rx->heads, nh);
	MemoryContextSwitchTo(old);
	pfree(v.data);
}

static void
tree_branch_delete(const char *name, bool force)
{
	ProllyBranchId id;
	ProllyHash	head;
	StringInfoData key;

	lock_refs();
	if (!branch_get(name, &id, &head))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly branch \"%s\" does not exist", name)));
	if (id == PROLLY_MAIN_BRANCH || id == prolly_session_branch())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot delete prolly branch \"%s\"", name),
				 errdetail(id == PROLLY_MAIN_BRANCH ?
						   "It is the database's default branch." :
						   "It is the session's branch.")));
	initStringInfo(&key);
	branch_key(name, &key);
	add_edit(key.data, key.len, NULL);
	pfree(key.data);
}

static void
tree_checkout(const char *name)
{
	ProllyBranchId id;
	ProllyHash	head;

	refs_init();
	if (!branch_get(name, &id, &head))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly branch \"%s\" does not exist", name)));
	prolly_set_session_branch(id);
}

/* Write a commit of branch name's working set wr; make it the head. */
static void
record_commit(const char *name, ProllyBranchId branch, const char *message,
			  const ProllyHash *parents, int nparents,
			  const ProllyNodeRef *wr, ProllyHash *commit_id)
{
	ProllyHash	local = wr->hash;
	ProllyDatabaseComponents dc;
	ProllyNodeRef canonical;
	StringInfoData key;
	StringInfoData v;

	/* The commit's root: the working set's canonical form. */
	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, canonical_root, wr, &canonical);
	write_commit(&canonical.hash, parents, nparents,
				 prolly_commit_author(), message,
				 GetCurrentTransactionStartTimestamp(), commit_id);

	initStringInfo(&key);
	initStringInfo(&v);
	commit_key(commit_id, &key);
	put_hash(&v, &local);
	add_edit(key.data, key.len, &v);
	branch_put(name, branch, commit_id);
	pfree(key.data);
	pfree(v.data);
}

static void
tree_commit(const char *message, ProllyHash *commit_id)
{
	ProllyBranchId branch = prolly_session_branch();
	char	   *name;
	ProllyBranchId id;
	ProllyHash	head;
	ProllyNodeRef wr;

	if (prolly_xact_has_writes())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot make a prolly commit in a transaction that has written to prolly tables"),
				 errhint("Commit the transaction's writes first.")));
	lock_refs();
	name = branch_name(branch);
	if (!branch_get(name, &id, &head))
		memset(&head, 0, sizeof(head));

	/*
	 * The branch's working set, with every table's rows made canonical:
	 * holding the branch's commit lock, the working set this transaction
	 * commits is the one recorded.
	 */
	{
		ProllyDatabaseComponents dc;

		prolly_database_components(&dc);
		PROLLY_CALL(comps.commit, branch_lock, branch);
		PROLLY_CALL(dc.canonicalizer, prepare);
		PROLLY_CALL(comps.engine, pending_root, &wr);
	}
	record_commit(name, branch, message, &head, head.len > 0 ? 1 : 0, &wr,
				  commit_id);
}

static void
tree_commit_with(const char *message, const ProllyHash *parents, int nparents,
				 const ProllyNodeRef *working_set, ProllyHash *commit_id)
{
	ProllyBranchId branch = prolly_session_branch();

	lock_refs();
	record_commit(branch_name(branch), branch, message, parents, nparents,
				  working_set, commit_id);
}

static bool
tree_commit_working_set(const ProllyHash *commit_id, ProllyNodeRef *root)
{
	ProllyHash	local;

	refs_init();
	if (!commit_local_root(commit_id, &local))
		return false;
	ref_of(&local, root);
	return true;
}

/* The newest common ancestor of a and b: breadth first from both. */
static bool
tree_merge_base(const ProllyHash *a, const ProllyHash *b, ProllyHash *base)
{
	List	   *seen = NIL;
	List	   *queue = NIL;
	ListCell   *lc;
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
		if (!tree_commit_get(h, &info))
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

		h = linitial(queue);
		queue = list_delete_first(queue);
		foreach(lc, seen)
		{
			ProllyHash *s = lfirst(lc);

			if (s->len == h->len && memcmp(s->bytes, h->bytes, h->len) == 0)
			{
				*base = *h;
				return true;
			}
		}
		if (!tree_commit_get(h, &info))
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

/*
 * Everything the refs keep alive: the refs tree, every commit's tree, the
 * working set it recorded, and its root.
 */
static void
tree_walk(const ProllyNodeRef *refs, ProllyTreeVisitNode visit, void *arg)
{
	ProllyTreeCursor *c;
	uint8		tag = TAG_COMMIT;
	ProllyKey	from = {&tag, 1};
	ProllyKey	k;
	ProllyValue v;
	List	   *commits = NIL;
	ListCell   *lc;

	refs_init();
	PROLLY_CALL(comps.tree, walk, &env, refs, visit, arg);
	c = PROLLY_CALL(comps.tree, cursor_open, &env, refs, CurrentMemoryContext);
	PROLLY_CALL(comps.tree, cursor_seek, c, &from, ForwardScanDirection);
	while (PROLLY_CALL(comps.tree, cursor_next, c, &k, &v))
	{
		ProllyHash *pair;

		if (k.len < 1 || k.data[0] != TAG_COMMIT)
			break;
		pair = palloc0(sizeof(ProllyHash) * 2);
		pair[0].len = k.len - 1;
		memcpy(pair[0].bytes, k.data + 1, k.len - 1);
		(void) get_hash((const char *) v.data, (const char *) v.data + v.len,
						&pair[1]);
		commits = lappend(commits, pair);
	}
	PROLLY_CALL(comps.tree, cursor_close, c);
	foreach(lc, commits)
	{
		ProllyHash *pair = lfirst(lc);
		ProllyNodeRef ref;
		StringInfoData rv;

		CHECK_FOR_INTERRUPTS();
		ref_of(&pair[0], &ref);
		PROLLY_CALL(comps.tree, walk, &env, &ref, visit, arg);
		initStringInfo(&rv);
		if (read_item(&ref, "root", &rv))
		{
			ProllyHash	r;
			ProllyNodeRef rref;
			ProllyDatabaseComponents dc;

			(void) get_hash(rv.data, rv.data + rv.len, &r);
			ref_of(&r, &rref);
			prolly_database_components(&dc);
			PROLLY_CALL(dc.canonicalizer, walk, &rref, visit, arg);
		}
		pfree(rv.data);
		ref_of(&pair[1], &ref);
		PROLLY_CALL(comps.root_layout, walk, &root_env, &ref, visit, arg);
	}
}

/* ------------------------------------------------------------------------
 * Listing, working sets and reset
 * ------------------------------------------------------------------------
 */

typedef struct BranchEntry
{
	char	   *name;
	ProllyHash	head;
	bool		gone;
} BranchEntry;

static int
branch_entry_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(((BranchEntry *) lfirst(a))->name,
				  ((BranchEntry *) lfirst(b))->name);
}

static BranchEntry *
find_entry(List *entries, const char *name)
{
	ListCell   *lc;

	foreach(lc, entries)
	{
		BranchEntry *be = lfirst(lc);

		if (strcmp(be->name, name) == 0)
			return be;
	}
	return NULL;
}

/* The branches committed, with the transaction's changes over them. */
static void
tree_branches(ProllyBranchVisit visit, void *arg)
{
	ProllyNodeRef root;
	List	   *entries = NIL;
	ListCell   *lc;
	BranchEntry *be;

	refs_init();
	if (refs_root(&root))
	{
		ProllyTreeCursor *c = PROLLY_CALL(comps.tree, cursor_open, &env, &root,
										  CurrentMemoryContext);
		uint8		tag = TAG_BRANCH;
		ProllyKey	from = {&tag, 1};
		ProllyKey	k;
		ProllyValue v;

		PROLLY_CALL(comps.tree, cursor_seek, c, &from, ForwardScanDirection);
		while (PROLLY_CALL(comps.tree, cursor_next, c, &k, &v))
		{
			if (k.len < 1 || k.data[0] != TAG_BRANCH)
				break;
			be = palloc0(sizeof(BranchEntry));
			be->name = pnstrdup((const char *) k.data + 1, k.len - 1);
			if (v.len >= 4)
				(void) get_hash((const char *) v.data + 4,
								(const char *) v.data + v.len, &be->head);
			entries = lappend(entries, be);
		}
		PROLLY_CALL(comps.tree, cursor_close, c);
	}
	if (rx)
	{
		foreach(lc, rx->edits)
		{
			RefEdit    *e = lfirst(lc);
			char	   *name;

			if (e->key.len < 1 || e->key.data[0] != TAG_BRANCH)
				continue;
			name = pnstrdup(e->key.data + 1, e->key.len - 1);
			be = find_entry(entries, name);
			if (be == NULL)
			{
				be = palloc0(sizeof(BranchEntry));
				be->name = name;
				entries = lappend(entries, be);
			}
			be->gone = e->is_delete;
			memset(&be->head, 0, sizeof(be->head));
			if (!e->is_delete && e->value.len >= 4)
				(void) get_hash(e->value.data + 4, e->value.data + e->value.len,
								&be->head);
		}
	}
	if (find_entry(entries, "main") == NULL)
	{
		be = palloc0(sizeof(BranchEntry));
		be->name = pstrdup("main");
		entries = lappend(entries, be);
	}
	list_sort(entries, branch_entry_cmp);
	foreach(lc, entries)
	{
		be = lfirst(lc);
		if (!be->gone)
			visit(be->name, &be->head, arg);
	}
}

static bool
tree_working_set(const char *branch, ProllyNodeRef *root)
{
	ProllyBranchId id;
	ProllyHash	head;

	refs_init();
	if (branch == NULL)
		id = prolly_session_branch();
	else if (!branch_get(branch, &id, &head))
		return false;
	if (!working_root(id, root))
		PROLLY_CALL(comps.root_layout, empty, &root_env, root);
	return true;
}

static void
tree_reset(const char *ref)
{
	ProllyBranchId branch = prolly_session_branch();
	ProllyHash	target;
	ProllyHash	root;
	NewHead    *nh;
	MemoryContext old;

	if (prolly_xact_has_writes())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot reset a prolly branch in a transaction that has written to prolly tables")));
	lock_refs();
	if (!tree_resolve(ref, &target))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	if (!commit_local_root(&target, &root))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit has no working set")));

	/* Nothing else commits to the branch until this transaction ends. */
	PROLLY_CALL(comps.commit, branch_lock, branch);
	branch_put(branch_name(branch), branch, &target);
	old = MemoryContextSwitchTo(rx->mcxt);
	nh = palloc(sizeof(NewHead));
	nh->branch = branch;
	nh->root = root;
	rx->heads = lappend(rx->heads, nh);
	MemoryContextSwitchTo(old);
}

/* ------------------------------------------------------------------------
 * Transaction end
 * ------------------------------------------------------------------------
 */

static int
edit_cmp(const ListCell *a, const ListCell *b)
{
	const RefEdit *ea = lfirst(a);
	const RefEdit *eb = lfirst(b);
	ProllyKey	ka = {(const uint8 *) ea->key.data, ea->key.len};
	ProllyKey	kb = {(const uint8 *) eb->key.data, eb->key.len};

	return memcmp_keys(&ka, &kb, NULL);
}

static void
refs_precommit(void)
{
	const ProllyCommitRoutine *commit = comps.commit;
	TransactionId xid;
	ListCell   *lc;

	if (rx->edits == NIL && rx->heads == NIL)
		return;
	xid = GetCurrentTransactionId();

	/* New branches' working sets, and reset ones'. */
	foreach(lc, rx->heads)
	{
		NewHead    *nh = lfirst(lc);
		ProllyNodeRef root;

		ref_of(&nh->root, &root);
		PROLLY_CALL(commit, branch_lock, nh->branch);
		if (rx->ntickets >= lengthof(rx->tickets))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("too many prolly branches created in one transaction")));
		rx->tickets[rx->ntickets++] = PROLLY_CALL(commit, enqueue, nh->branch,
												  &root, xid);
		PROLLY_CALL(commit, branch_unlock, nh->branch);
	}

	/* The refs: the last change to each key, applied in key order. */
	if (rx->edits != NIL)
	{
		List	   *sorted = list_copy(rx->edits);
		ProllyEdit *edits = palloc(sizeof(ProllyEdit) * list_length(sorted));
		int			n = 0;
		ProllyNodeRef root;
		ProllyNodeRef new_root;

		/* A stable sort keeps the order made for each key; keep the last. */
		list_sort(sorted, edit_cmp);
		foreach(lc, sorted)
		{
			RefEdit    *e = lfirst(lc);
			ProllyKey	k = {(const uint8 *) e->key.data, e->key.len};

			if (n > 0 && memcmp_keys(&edits[n - 1].key, &k, NULL) == 0)
				n--;
			edits[n].key = k;
			edits[n].value.data = (const uint8 *) e->value.data;
			edits[n].value.len = e->value.len;
			edits[n].is_delete = e->is_delete;
			n++;
		}
		if (!PROLLY_CALL(commit, tip, PROLLY_REFS_BRANCH, &root))
			empty_tree(&root);
		PROLLY_CALL(comps.tree, apply, &env, &root, edits, n, &new_root);
		rx->tickets[rx->ntickets++] = PROLLY_CALL(commit, enqueue,
												  PROLLY_REFS_BRANCH,
												  &new_root, xid);
		PROLLY_CALL(commit, branch_unlock, PROLLY_REFS_BRANCH);
	}
}

static void
refs_xact_callback(XactEvent event, void *arg)
{
	if (rx == NULL)
		return;
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			refs_precommit();
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
			for (int i = 0; i < rx->ntickets; i++)
			{
				if (event == XACT_EVENT_COMMIT)
					PROLLY_CALL(comps.commit, publish, rx->tickets[i]);
				else
					PROLLY_CALL(comps.commit, abandon, rx->tickets[i]);
			}
			rx = NULL;
			break;
		case XACT_EVENT_PRE_PREPARE:
			if (rx->edits != NIL || rx->heads != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot PREPARE a transaction that has changed prolly branches or commits")));
			break;
		case XACT_EVENT_PREPARE:
			rx = NULL;
			break;
		default:
			break;
	}
}

const ProllyRefsRoutine prolly_refs_tree = {
	.info = {
		.iface = PROLLY_IFACE_REFS,
		.impl_id = 1,
		.format_version = 1,
		.name = "tree",
		.description = "branches and commits in a prolly tree, the head of a "
		"reserved branch",
	},
	.branch_create = tree_branch_create,
	.branch_delete = tree_branch_delete,
	.branch_lookup = tree_branch_lookup,
	.checkout = tree_checkout,
	.commit = tree_commit,
	.resolve = tree_resolve,
	.commit_get = tree_commit_get,
	.merge_base = tree_merge_base,
	.commit_working_set = tree_commit_working_set,
	.commit_with = tree_commit_with,
	.branches = tree_branches,
	.working_set = tree_working_set,
	.reset = tree_reset,
	.walk = tree_walk,
};
