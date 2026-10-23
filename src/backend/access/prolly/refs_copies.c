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
 *				   common ancestor (hash length, hash each), the count of
 *				   tables with unresolved conflicts and their OIDs (4 bytes
 *				   each, big-endian), then, if it is a cherry-pick, 'p'
 *				   and the picked commit's message, or, a revert's, 'r',
 *				   the head before its series (hash length, hash), the
 *				   count of commits still to revert and their ids (hash
 *				   length, hash each), and its commit's message
 *	'V' name	-> the branch's constraint violations: for each, its
 *				   table's OID (4 bytes, big-endian), then its type, row and
 *				   what it violates, each a length (4 bytes, big-endian) and
 *				   UTF-8 text
 *	'O' name	-> a remote: its URL
 *	'R' remote NUL branch
 *				-> a remote-tracking branch: the commit a fetch last found
 *				   the remote's branch at (hash length, hash)
 *	'U' name	-> the branch's upstream: a remote, NUL, its branch
 *	'X' name	-> the transaction that last moved the branch's head (its
 *				   FullTransactionId, native byte order; a TransactionId,
 *				   as noted before)
 *	'Y' name	-> the transaction that last moved its baseline (the same)
 *	'C' name	-> a database setting's value (text)
 *	'N' name	-> the head's tables, as the commit that made it of the
 *				   working set named them: the head's hash, then a line
 *				   per table (its OID, then its qualified name)
 *	'Q' name	-> the branch's merge's conflicted tables' names, as the
 *				   merge recorded them: each table's OID, then its name's
 *				   length (a varint) and bytes
 *	'Z' name	-> a list of stashes, newest first: each its commit (hash
 *				   length, hash), then its branch's name (a varint length,
 *				   then the bytes)
 *
 * A branch without an entry has no commits yet.  A transaction can't
 * commit a merge with unresolved conflicts, or write to a branch that has
 * them, unless dolt.allow_commit_conflicts is on.  A commit is a tree of its
 * own (keys by memcmp), and its id is that tree's root hash, so the same
 * commit content has the same id in every database:
 *
 *	"author"	-> the author
 *	"dependencies" -> what the versioned objects needed from outside
 *				   the versioned schemas (length byte, hash of the
 *				   canonicalizer's tree of them); a commit without it
 *				   (one made before they were recorded) has them unknown
 *	"height"	-> its height (prolly_history_height()), 8 bytes
 *				   big-endian: 1 + the most of its parents', a root's 1;
 *				   a commit without it (one made before heights were
 *				   recorded) has its height found from its parents'
 *	"message"	-> the message
 *	"parents"	-> the parents' ids, each a length byte and the hash
 *	"root"		-> the commit's root (length byte, hash): the canonical
 *				   form of the branch's versioned schemas, folded
 *	"time"		-> microseconds since the Postgres epoch, 8 bytes big-endian
 *
 * The chunks a commit's items and the refs' entries name (its dependencies,
 * parents and root; a branch's head, baseline and working set, a merge's
 * commits, a remote branch's head) are declared to the tree as the
 * references to chunks its leaves hold (commit_chunk_refs(),
 * refs_chunk_refs()), so
 * a collection follows them without reading values.
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
 * Every change of a branch's own entries (its head, its baseline, the
 * names its head's tables had, its merge state and violations) is made
 * holding that branch's lock (lock_branch()) to transaction end, and a
 * transaction that moves the root holds the session's branch's too,
 * taken first.  A commit (or a checkpoint) changes only its branch's
 * entries, and holds only its lock: so commits on different branches run
 * together, while those on one branch take turns.  Its changes are made
 * to a tree of its own, built on the root as it last read it and rebuilt
 * on the root whenever another's commit moved it, and the store's root is
 * moved there only as the transaction commits, waiting then for a mover
 * holding it.  Such a transaction that comes to change another entry
 * moves the root from then on, its changes moved there at once.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/refs_copies.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/prolly_builtins.h"
#include "utils/fmgroids.h"
#include "catalog/pg_branch.h"
#include "access/prolly_canon.h"
#include "access/prolly_changelog.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_config.h"
#include "access/prolly_datum.h"
#include "access/prolly_deps.h"
#include "access/prolly_fold.h"
#include "access/prolly_xact.h"
#include "access/xact.h"
#include "access/amapi.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "catalog/branch.h"
#include "catalog/branch_ops.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_branch_d.h"
#include "catalog/pg_class.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "refs_generations.h"
#include "storage/bufmgr.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#define TAG_BRANCH		'B'
#define TAG_MERGE		'M'
#define TAG_BASELINE	'W'
#define TAG_VIOLATIONS	'V'
#define TAG_REMOTE		'O'
#define TAG_TRACKING	'R'
#define TAG_UPSTREAM	'U'
#define TAG_MOVER		'X'	/* the transaction that last moved a head */
#define TAG_BASELINE_MOVER 'Y'	/* and a baseline (a fold, a checkpoint) */
#define TAG_CONFIG		'C'	/* a database setting */
#define TAG_NAMES		'N'	/* the head's tables, as named by a full commit */
#define TAG_MERGE_NAMES	'Q'	/* a merge's conflicted tables' names then */
#define TAG_STASH		'Z'	/* a list of stashes, by name */

/*
 * A branch's own lock (an object lock in the database, of this class, by a
 * hash of the branch's name: two names sharing one only take turns), held
 * by whoever changes the branch's entries, to transaction end.
 */
#define BRANCH_LOCK_CLASSID	((Oid) 0x50524642)

static bool ready = false;

/*
 * A transaction holding single branches: whether it has changes to them
 * not yet on the store's root (scoped), the tree they are in (mine), and
 * the root that tree was built on (base).  The changes are moved to the
 * root as the transaction commits (published, from then until it ends:
 * anything changed later is moved to the root at once).  Each
 * subtransaction that changes them saves them first (ScopedSave), so its
 * abort restores them.
 */
typedef struct ScopedSave
{
	int			level;
	bool		scoped;
	bool		whole;
	ProllyHash	base;
	ProllyHash	mine;
} ScopedSave;

static bool scoped = false;
static bool whole = false;		/* the transaction holds every ref */
static ProllyHash scoped_base;
static ProllyHash scoped_mine;
static bool published = false;
static List *scoped_saved = NIL;	/* ScopedSave *, innermost last */

/*
 * The branches whose merge states the transaction set (names, in
 * TopTransactionContext): checked when it commits or prepares, whichever
 * branch it is on then.
 */
static List *merge_changed = NIL;

static void refs_xact_callback(XactEvent event, void *arg);
static void refs_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
								  SubTransactionId parentSubid, void *arg);
static void publish_scoped(void);
static bool merge_state_of(const char *branch, ProllyMergeState *state);
static void merge_tables_left(const char *branch, ProllyMergeState *state,
							  char **name);
static List *violations_left(const char *branch, List *violations,
							 char **name);
static Oid	branch_oid(const char *name);
static void merge_key(StringInfo key, const char *branch);
static void merge_names_put(const char *branch, const ProllyMergeState *state);
static void merge_names_get(const char *branch, ProllyMergeState *state);
static List *violations_of(const char *branch);
static void violations_key(StringInfo key, const char *branch);
static List *copies_violations(void);
static bool tracking_head(const char *refname, ProllyHash *commit_id);
static bool upstream_head(const char *branch, ProllyHash *commit_id);
static void copies_set_violations(List *violations);
static bool copies_merge_state(ProllyMergeState *state);
static void copies_set_merge_state(const ProllyMergeState *state);
static bool copies_commit_working_set(const ProllyHash *commit_id,
									  ProllyNodeRef *root);
static bool copies_working_set(const char *branch, ProllyNodeRef *root);
static uint32 get_be32(const char *p);
static void fold_from_baseline(ProllyNodeRef *root, ProllyNodeRef *next, bool commit);
static const ProllyTreeRoutine *tree;
static ProllyTreeEnv env;		/* the refs' tree, and commits read */
static ProllyTreeEnv commit_env;	/* commits written */

static const char *get_hash(const char *p, const char *end, ProllyHash *h);

/* Declare each hash at p (a length byte, then the hash), n of them. */
static const char *
declare_hashes(const char *p, const char *end, int n, StringInfo refs)
{
	for (int i = 0; i < n && p < end; i++)
	{
		ProllyHash	h;

		p = get_hash(p, end, &h);
		if (h.len > 0)
			PROLLY_CALL(tree, value_reference, &h, 0, refs);
	}
	return p;
}

/* A commit's items: its dependencies, parents and root name chunks. */
static void
commit_chunk_refs(const ProllyKey *key, const ProllyValue *value, void *arg,
				  StringInfo refs)
{
	const char *p = (const char *) value->data;
	const char *end = p + value->len;

#define KEY_IS(name) \
	(key->len == strlen(name) && memcmp(key->data, name, key->len) == 0)
	if (KEY_IS("dependencies") || KEY_IS("parents") || KEY_IS("root"))
		(void) declare_hashes(p, end, INT_MAX, refs);
#undef KEY_IS
}

/*
 * The refs' entries: a branch's head, a remote branch's, a baseline and the
 * working set its fold made, a merge's commits (theirs and the base; a
 * revert's series head and the commits it has left), and each stash's
 * commit.
 */
static void
refs_chunk_refs(const ProllyKey *key, const ProllyValue *value, void *arg,
				StringInfo refs)
{
	const char *p = (const char *) value->data;
	const char *end = p + value->len;

	if (key->len < 1)
		return;
	switch (key->data[0])
	{
		case TAG_BRANCH:
		case TAG_TRACKING:
			(void) declare_hashes(p, end, 1, refs);
			break;
		case TAG_BASELINE:
			(void) declare_hashes(p, end, 2, refs);
			break;
		case TAG_MERGE:
			{
				uint32		n;

				p = declare_hashes(p, end, 2, refs);
				if (end - p < 4)
					break;
				n = get_be32(p);
				p += 4;
				if ((uint64) (end - p) < (uint64) n * 4)
					break;
				p += (Size) n * 4;
				if (p < end && *p == 'r')
				{
					p = declare_hashes(p + 1, end, 1, refs);
					if (end - p < 4)
						break;
					n = get_be32(p);
					(void) declare_hashes(p + 4, end, (int) Min(n, INT_MAX), refs);
				}
			}
			break;
		case TAG_STASH:
			/* (each entry: its commit, then its branch's name) */
			while (p < end)
			{
				uint64		blen;

				p = declare_hashes(p, end, 1, refs);
				blen = prolly_get_varint(&p, end);
				if (blen > (uint64) (end - p))
					break;
				p += blen;
			}
			break;
		default:
			break;
	}
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
	env.hash = prolly_content_hash();
	env.chunk_store = prolly_chunk_store();
	env.chunks = PROLLY_CALL(env.chunk_store, open, MyDatabaseId,
							 MyDatabaseTableSpace);
	env.compare = prolly_bytes_compare;
	env.counters = pgstat_prolly_tree_counters();
	commit_env = env;
	env.chunk_refs = refs_chunk_refs;
	commit_env.chunk_refs = commit_chunk_refs;
	RegisterXactCallback(refs_xact_callback, NULL);
	RegisterSubXactCallback(refs_subxact_callback, NULL);
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

/* The transaction set branch's merge state, or violations. */
static void
note_merge_changed(const char *branch)
{
	if (!name_listed(merge_changed, branch))
	{
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

		merge_changed = lappend(merge_changed, pstrdup(branch));
		MemoryContextSwitchTo(old);
	}
}

typedef struct TablesExist
{
	int			n;
	const Oid  *relids;
	bool	   *exists;			/* out: whether each is a table there */
	char	  **name;			/* out, if not NULL: the first one's name */
} TablesExist;

static void
tables_exist_here(void *arg)
{
	TablesExist *te = arg;

	if (te->name)
		*te->name = NULL;
	for (int i = 0; i < te->n; i++)
	{
		te->exists[i] = SearchSysCacheExists1(RELOID,
											  ObjectIdGetDatum(te->relids[i]));
		if (te->exists[i] && te->name && *te->name == NULL)
			*te->name = get_rel_name(te->relids[i]);
	}
}

/*
 * Which of relids are tables on branch, as its own catalog has them: a
 * merge state, or violations, of a branch other than the session's names
 * tables of that branch, which the session's branch may not have (or may
 * have dropped, where that one still has them).  A branch with only a head
 * has no catalog of its own yet, and so nothing dropped from it.
 */
static void
tables_exist_on(const char *branch, int n, const Oid *relids, bool *exists,
				char **name)
{
	TablesExist te = {n, relids, exists, name};

	if (n == 0)
	{
		if (name)
			*name = NULL;
		return;
	}
	if (strcmp(branch, branch_ops()->current()) == 0)
		tables_exist_here(&te);
	else if (OidIsValid(branch_oid(branch)))
		branch_ops()->run_on(branch, tables_exist_here, &te);
	else
	{
		for (int i = 0; i < n; i++)
			exists[i] = true;
		if (name)
			*name = get_rel_name(relids[0]);
	}
}

typedef struct NameBranch
{
	Oid			branch;
	char	   *name;
} NameBranch;

static void
name_branch(const char *name, Oid branch, void *arg)
{
	NameBranch *nb = arg;

	if (branch == nb->branch)
		nb->name = pstrdup(name);
}

/*
 * The branches whose unresolved conflicts or violations the transaction
 * can't commit: those whose merge states it set, and those it wrote, as
 * Dolt refuses a commit of a working set with conflicts whoever made them
 * (#587).  Names, palloc'd.
 */
static List *
branches_to_check(void)
{
	List	   *names = list_copy(merge_changed);
	ListCell   *lc;

	foreach(lc, branches_written())
	{
		NameBranch	nb = {lfirst_oid(lc), NULL};

		if (nb.branch == MyBranch)
			nb.name = branch_ops()->current();
		else
			branch_ops()->branches(name_branch, &nb);
		if (nb.name != NULL && !name_listed(names, nb.name))
			names = lappend(names, nb.name);
	}
	return names;
}

/*
 * Unresolved conflicts, or constraint violations, aren't committed: on
 * each branch the transaction made a merge on or wrote, as that branch
 * has its tables (a checkout since doesn't hide them).
 */
static void
check_merge_conflicts(const char *what)
{
	ListCell   *lc;

	if (prolly_allow_commit_conflicts())
		return;
	foreach(lc, branches_to_check())
	{
		const char *branch = lfirst(lc);
		ProllyMergeState ms;
		List	   *v;
		char	   *name = NULL;

		if (merge_state_of(branch, &ms))
			merge_tables_left(branch, &ms, &name);
		if (ms.ntables > 0)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("cannot %s a transaction with unresolved merge conflicts", what),
					 errdetail("Table \"%s\" has conflicts on branch \"%s\".",
							   name, branch),
					 errhint("Resolve them with dolt_conflicts_resolve() first, or set dolt.allow_commit_conflicts to keep them.")));
		v = violations_left(branch, violations_of(branch), &name);
		if (v != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("cannot %s a transaction with constraint violations", what),
					 errdetail("Table \"%s\" has a row with a %s violation on branch \"%s\".",
							   name,
							   ((ProllyViolation *) linitial(v))->type,
							   branch),
					 errhint("Fix the rows and check them again with dolt_verify_constraints(), or set dolt.allow_commit_conflicts to keep them.")));
	}
}

static void
refs_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			check_merge_conflicts("commit");
			if (prolly_refs_end_hook)
				prolly_refs_end_hook(false);
			publish_scoped();
			published = true;
			break;
		case XACT_EVENT_PRE_PREPARE:
			check_merge_conflicts("prepare");
			if (prolly_refs_end_hook)
				prolly_refs_end_hook(true);
			publish_scoped();
			published = true;
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
			merge_changed = NIL;
			scoped = false;
			whole = false;
			published = false;
			scoped_saved = NIL;
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

/* Save the scoped changes for the subtransaction, before changing them. */
static void
scoped_save(void)
{
	int			level = GetCurrentTransactionNestLevel();
	ScopedSave *save;
	MemoryContext old;

	if (level <= 1 ||
		(scoped_saved != NIL && ((ScopedSave *) llast(scoped_saved))->level >= level))
		return;
	old = MemoryContextSwitchTo(TopTransactionContext);
	save = palloc(sizeof(ScopedSave));
	save->level = level;
	save->scoped = scoped;
	save->whole = whole;
	save->base = scoped_base;
	save->mine = scoped_mine;
	scoped_saved = lappend(scoped_saved, save);
	MemoryContextSwitchTo(old);
}

static void
refs_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					  SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();
	ScopedSave *last;

	if (scoped_saved == NIL)
		return;
	last = llast(scoped_saved);
	if (last->level < level)
		return;
	Assert(last->level == level);
	if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		scoped = last->scoped;
		whole = last->whole;
		scoped_base = last->base;
		scoped_mine = last->mine;
		scoped_saved = list_delete_last(scoped_saved);
	}
	else if (event == SUBXACT_EVENT_COMMIT_SUB)
	{
		/* the parent's saved state, if it has one, is the older */
		if (level - 1 <= 1 ||
			(list_length(scoped_saved) > 1 &&
			 ((ScopedSave *) list_nth(scoped_saved,
									  list_length(scoped_saved) - 2))->level == level - 1))
			scoped_saved = list_delete_last(scoped_saved);
		else
			last->level = level - 1;
	}
}

typedef struct ScopedEdits
{
	ProllyEdit *edits;
	int			n;
	int			max;
} ScopedEdits;

static void
collect_edit(ProllyDiffKind kind, const ProllyKey *key,
			 const ProllyValue *before, const ProllyValue *after, void *arg)
{
	ScopedEdits *se = arg;
	ProllyEdit *e;
	uint8	   *k;

	if (se->n == se->max)
	{
		se->max = Max(8, se->max * 2);
		se->edits = se->edits ? repalloc_array(se->edits, ProllyEdit, se->max) :
			palloc_array(ProllyEdit, se->max);
	}
	e = &se->edits[se->n++];
	k = palloc(Max(key->len, 1));
	memcpy(k, key->data, key->len);
	e->key.data = k;
	e->key.len = key->len;
	e->is_delete = (after == NULL);
	e->value.data = NULL;
	e->value.len = 0;
	if (after)
	{
		uint8	   *v = palloc(Max(after->len, 1));

		memcpy(v, after->data, after->len);
		e->value.data = v;
		e->value.len = after->len;
	}
}

/*
 * The scoped changes, rebuilt on cur (the store's root now) if another's
 * commit moved it since they were built: the entries they changed are the
 * transaction's branches', which no one else changes meanwhile.
 */
static void
rebase_scoped(const ProllyHash *cur)
{
	ScopedEdits se = {NULL, 0, 0};
	ProllyNodeRef from;
	ProllyNodeRef to;
	ProllyNodeRef onto;
	ProllyNodeRef next;

	if (same_hash(cur, &scoped_base))
		return;
	scoped_save();
	ref_of(&scoped_base, &from);
	ref_of(&scoped_mine, &to);
	PROLLY_CALL(tree, diff, &env, &from, &to, collect_edit, &se);
	if (se.n == 0)
		scoped_mine = *cur;
	else
	{
		ref_of(cur, &onto);
		PROLLY_CALL(tree, apply, &env, &onto, se.edits, se.n, &next);
		scoped_mine = next.hash;
	}
	scoped_base = *cur;
	for (int i = 0; i < se.n; i++)
	{
		pfree(unconstify(uint8 *, se.edits[i].key.data));
		if (se.edits[i].value.data)
			pfree(unconstify(uint8 *, se.edits[i].value.data));
	}
	if (se.edits)
		pfree(se.edits);
}

/*
 * Move the store's root to the scoped changes, with the transaction (as it
 * commits, or as it comes to hold every ref): waiting only for another
 * moving it so.
 */
static void
publish_scoped(void)
{
	if (!scoped)
		return;
	scoped_save();
	for (;;)
	{
		ProllyHash	cur;

		PROLLY_CALL(env.chunk_store, root, env.chunks, &cur);
		rebase_scoped(&cur);
		if (PROLLY_CALL(env.chunk_store, move_root, env.chunks, &cur,
						&scoped_mine))
			break;
	}
	scoped = false;
}

/* The refs tree's root, as the transaction sees it; false if none yet. */
static bool
refs_root(ProllyNodeRef *root)
{
	ProllyHash	h;

	refs_init();
	PROLLY_CALL(env.chunk_store, root, env.chunks, &h);
	if (scoped)
	{
		rebase_scoped(&h);
		h = scoped_mine;
	}
	if (h.len == 0)
		return false;
	ref_of(&h, root);
	return true;
}

static void
branch_lock_tag(LOCKTAG *tag, const char *name, int len)
{
	SET_LOCKTAG_OBJECT(*tag, MyDatabaseId, BRANCH_LOCK_CLASSID,
					   hash_bytes((const unsigned char *) name, len), 0);
}

/* Does the transaction hold the lock of the branch named (len bytes)? */
static bool
branch_held(const char *name, int len)
{
	LOCKTAG		tag;

	branch_lock_tag(&tag, name, len);
	return LockHeldByMe(&tag, ExclusiveLock, false);
}

/*
 * Hold branch name's refs until the transaction ends, for changing its
 * entries: other holders of name are waited for, and wait for it, but
 * holders of other branches don't.
 */
static void
lock_branch(const char *name)
{
	LOCKTAG		tag;

	refs_init();
	PreventCommandIfReadOnly("moving a chunk store's root");
	branch_lock_tag(&tag, name, strlen(name));
	(void) LockAcquire(&tag, ExclusiveLock, false, false);
}

/*
 * lock_branch(), if no one holds the branch now (never waiting): false,
 * nothing held, if someone does.  With hold false, only whether it is
 * free is told, nothing being kept.
 */
static bool
try_lock_branch(const char *name, bool hold)
{
	LOCKTAG		tag;

	refs_init();
	branch_lock_tag(&tag, name, strlen(name));
	if (LockAcquire(&tag, ExclusiveLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
		return false;
	if (!hold)
		LockRelease(&tag, ExclusiveLock, false);
	return true;
}

/*
 * Hold every ref until the transaction ends, the session's branch's own
 * too: move the root to itself (or, in a new store, to an empty refs
 * tree).  A mover that comes first is waited for, and then built on;
 * holders of other branches aren't, but they wait to commit.  Scoped
 * changes of this transaction's are moved to the root first.  (Holding
 * them already, this moves the root to itself again, which the
 * transaction's move already is.)
 */
static void
lock_refs(void)
{
	refs_init();
	if (!published && !whole)
	{
		lock_branch(branch_ops()->current());
		scoped_save();
		whole = true;
	}
	publish_scoped();
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

/*
 * lock_refs(), with branch name the session's, if every lock it takes is
 * free now (never waiting): false if one isn't.  (One taken before
 * another is found held stays held, to no effect: nothing is changed by
 * it.)
 */
static bool
try_hold_refs(const char *name)
{
	if (!whole && !published &&
		(!try_lock_branch(name, true) ||
		 !PROLLY_HAS(env.chunk_store, try_hold_root) ||
		 !env.chunk_store->try_hold_root(env.chunks)))
		return false;
	lock_refs();
	return true;
}

/* Is key one of a branch's own entries?  Its name then follows the tag. */
static bool
branch_entry(const char *key, int keylen)
{
	if (keylen < 2)
		return false;
	switch (key[0])
	{
		case TAG_BRANCH:
		case TAG_MOVER:
		case TAG_BASELINE:
		case TAG_BASELINE_MOVER:
		case TAG_NAMES:
		case TAG_MERGE:
		case TAG_MERGE_NAMES:
		case TAG_VIOLATIONS:
			return true;
		default:
			return false;
	}
}

/*
 * May the entry key be changed in the scoped changes: one of a branch the
 * transaction holds, not having held every ref nor begun to commit?
 */
static bool
scoped_key(const char *key, int keylen)
{
	return !published && !whole && branch_entry(key, keylen) &&
		branch_held(key + 1, keylen - 1);
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

	refs_init();
	e.key.data = (const uint8 *) key;
	e.key.len = keylen;
	e.is_delete = (value == NULL);
	e.value.data = value ? (const uint8 *) value->data : NULL;
	e.value.len = value ? value->len : 0;
	if (scoped_key(key, keylen))
	{
		ProllyHash	cur;

		PROLLY_CALL(env.chunk_store, root, env.chunks, &cur);
		if (scoped)
			rebase_scoped(&cur);
		/* (a store with no root yet gets one from a holder of every ref) */
		if (scoped || cur.len > 0)
		{
			scoped_save();
			if (!scoped)
			{
				scoped_base = cur;
				scoped_mine = cur;
				scoped = true;
			}
			ref_of(&scoped_mine, &root);
			PROLLY_CALL(tree, apply, &env, &root, &e, 1, &new_root);
			scoped_mine = new_root.hash;
			return;
		}
	}
	lock_refs();

	/* (a branch's entries are changed only by a holder of its lock) */
	if (branch_entry(key, keylen) && !branch_held(key + 1, keylen - 1))
	{
		char	   *name = pnstrdup(key + 1, keylen - 1);

		lock_branch(name);
		pfree(name);
	}
	if (!refs_root(&root))
		elog(ERROR, "prolly refs have no root");
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
	FullTransactionId xid = head ? GetTopFullTransactionId() :
		InvalidFullTransactionId;

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
	if (head)
		pgstat_count_prolly(PGSTAT_PROLLY_HEAD_MOVES, 1);
}

/*
 * Did a transaction the active snapshot doesn't see move branch name's
 * head (or, with tag TAG_BASELINE_MOVER, its baseline)?  (A committer that
 * waited for the refs finds the head another moved, while its snapshot,
 * taken before, lacks that one's rows; or a baseline another's fold or
 * checkpoint wrote, which its snapshot can't fold from.)
 */
static bool
moved_unseen(char tag, const char *name)
{
	StringInfoData key;
	StringInfoData v;
	TransactionId xid = InvalidTransactionId;

	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, tag);
	appendStringInfoString(&key, name);
	if (refs_get(key.data, key.len, &v))
	{
		/*
		 * (a FullTransactionId; one 2^31 or more before the next is long
		 * past, and seen, though its 32 bits alone would look recent; a
		 * TransactionId, as heads' were noted before)
		 */
		if (v.len == sizeof(FullTransactionId))
		{
			FullTransactionId fxid;
			uint64		next = U64FromFullTransactionId(ReadNextFullTransactionId());

			memcpy(&fxid, v.data, sizeof(fxid));
			if (FullTransactionIdIsValid(fxid) &&
				next - U64FromFullTransactionId(fxid) < ((uint64) 1 << 31))
				xid = XidFromFullTransactionId(fxid);
		}
		else if (v.len == sizeof(xid))
			memcpy(&xid, v.data, sizeof(xid));
	}
	pfree(key.data);
	pfree(v.data);
	return TransactionIdIsValid(xid) && !TransactionIdIsCurrentTransactionId(xid) &&
		XidInMVCCSnapshot(xid, GetActiveSnapshot());
}

static bool
head_moved_unseen(const char *name)
{
	return moved_unseen(TAG_MOVER, name);
}

/*
 * With the refs held, a snapshot that sees the head's commit: the active
 * one, or (read committed) a newer one, pushed (true; the caller pops it).
 * A transaction whose snapshot can't move fails instead, to be retried,
 * as a commit on top of a head it can't see would undo that head's rows.
 * Read committed takes a newer one too when only the baseline moved since
 * its snapshot (another's checkpoint): the old one would see none of that
 * fold, and read every table in full, and leave each unsettled.
 */
static bool
see_head(const char *name)
{
	if (!head_moved_unseen(name))
	{
		if (IsolationUsesXactSnapshot() ||
			!moved_unseen(TAG_BASELINE_MOVER, name))
			return false;
		PushActiveSnapshot(GetLatestSnapshot());
		return true;
	}
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
 * known) is the working set's root its fold made.  The transaction that
 * moved it is noted (see_head()).
 */
static void
branch_set_baseline(const char *name, const ProllyNodeRef *baseline,
					const ProllyNodeRef *root)
{
	StringInfoData key;
	StringInfoData v;
	FullTransactionId xid = baseline ? GetTopFullTransactionId() :
		InvalidFullTransactionId;

	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_BASELINE);
	appendStringInfoString(&key, name);
	if (baseline)
		put_hash(&v, &baseline->hash);
	if (baseline && root)
		put_hash(&v, &root->hash);
	refs_put(key.data, key.len, baseline ? &v : NULL);

	resetStringInfo(&key);
	resetStringInfo(&v);
	appendStringInfoChar(&key, TAG_BASELINE_MOVER);
	appendStringInfoString(&key, name);
	appendBinaryStringInfo(&v, &xid, sizeof(xid));
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

/*
 * Was branch name's catalog row (made at its first use, with its copies)
 * written by a transaction the transaction's snapshot doesn't see?
 */
static bool
branch_made_unseen(const char *name)
{
	Relation	brel = table_open(BranchRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	bool		seen;

	ScanKeyInit(&key, Anum_pg_branch_brname, BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(name));
	scan = systable_beginscan(brel, BranchNameIndexId, true,
							  GetTransactionSnapshot(), 1, &key);
	seen = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(brel, AccessShareLock);
	return !seen;
}

/*
 * A branch is its head in the refs, and its copies of the versioned
 * schemas.  A branch made at a commit has only the head until it is first
 * used (checked out, or its working set read): making it is O(1), and one
 * never used costs nothing more.
 */
static bool
branch_exists(const char *name)
{
	ProllyHash	head;

	return OidIsValid(branch_oid(name)) || branch_head(name, &head);
}

static void working_fold(ProllyNodeRef *root);
static void fold_as_baseline(void *arg);
static bool known_working_set(const char *name, Oid branch,
							  const ProllyNodeRef *baseline, bool complete,
							  ProllyNodeRef *root);
static bool fold_caches_usable(void);
static Snapshot fold_snapshot(void);
static void verify_known(const char *name, bool complete,
						 const ProllyNodeRef *root);
static bool share_clean_tables(const ProllyNodeRef *target,
							   ProllyNodeRef *working);
static bool advance_to_target(const ProllyNodeRef *target,
							  const ProllyNodeRef *working);

/*
 * A first use's source whose working set was known to be the root its
 * copies were made at (name, the root, and what it was known at), taken
 * before the copies were made: their catalog rows keep anything from
 * being known after.
 */
typedef struct KnownSource
{
	const char *name;
	ProllyHash	root;
	BranchFoldStamp at;
} KnownSource;

/*
 * The working set of copies made from ks's branch, as it still is (#482):
 * false if it may have changed since it was known.
 */
static bool
copied_working_set(const KnownSource *ks, ProllyNodeRef *working)
{
	if (ks == NULL || !fold_caches_usable() || !branch_fold_unchanged(&ks->at))
		return false;
	ref_of(&ks->root, working);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLDS_REUSED, 1);
	if (debug_prolly_verify_fold_caches)
		verify_known(ks->name, false, working);
	return true;
}

/*
 * Move the session's branch to a commit's root, every move (a reset, a
 * fast-forward, a branch's first use): its rows written, unless its
 * working set is the root already; then folded from there, so no fold
 * after reads the pages the move wrote again and again.  Objects no
 * commit could hold (the canonical form doesn't describe them) are
 * uncommitted work the move discards, as it does any other.  The working
 * set is folded only if nothing known says what it is (#482): the
 * branch's, or (source, a first use's) the working set of the branch its
 * copies were made from, as they are still (#491).  Otherwise (a move of
 * the session's branch, or a first use whose source's working set wasn't
 * known to be the copies' root) the working set, folded, is the branch's
 * baseline from then on (a checkpoint), so that after the move the
 * baseline can be advanced to the target without folding again
 * (advance_to_target()): a move from a dirty working set folds it once.
 */
static void
settle(const ProllyNodeRef *target, const KnownSource *source)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef working;

	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, discard_undescribed);
	if (source != NULL)
	{
		if (!copied_working_set(source, &working))
			working_fold(&working);
	}
	else
	{
		char	   *name = branch_ops()->current();
		ProllyNodeRef baseline;
		bool		have = branch_baseline(name, &baseline);

		if (!known_working_set(name, MyBranch, have ? &baseline : NULL, false,
							   &working))
		{
			ProllyNodeRef next;

			fold_from_baseline(&working, &next, false);
			branch_set_baseline(name, &next, &working);
		}
	}
	if (same_hash(&working.hash, &target->hash))
		return;

	/* (the tables another branch has as the target given its pages) */
	if (share_clean_tables(target, &working) &&
		same_hash(&working.hash, &target->hash))
		return;
	PROLLY_CALL(dc.diff_merge, move_to, target, &working);
	if (!advance_to_target(target, &working))
		fold_as_baseline(NULL);
}

/*
 * Sharing pages instead of writing rows (#491).  A move to a root another
 * branch's working set is known to be (nobody writing it, every writer
 * that ended seen) gives each table whose rows differ that branch's
 * storage instead, the table's with its indexes', TOAST table's and change
 * log's, when sharing their blocks costs less than writing the rows that
 * differ (about 10 us a block against 11 a row): its pages, maps and log
 * then are that branch's, and the baseline takes that branch's entry for
 * it, which its log goes on from.  Only a move of rows (the two roots
 * define the same objects), only of tables whose relations are that
 * branch's too (made from the same, so the same ids), and only where the
 * locks are free now (share()'s: this branch's copies as TRUNCATE locks
 * them, that branch's against its writers), that branch still known at
 * the root as it was known once they are held (the first table shared
 * changes pg_class, after which nothing new is known in the transaction).
 * Not at a first use: the copies' catalog rows, made in the transaction,
 * keep anything from being known after them, and a branch known at the
 * root was copied already.  As TRUNCATE, it isn't MVCC-safe: a snapshot
 * taken before the move commits reads the shared storage after.
 */
#define MOVE_ROW_USEC		11
#define SHARE_BLOCK_USEC	10

/* A branch other than the session's whose working set is known to be root. */
typedef struct CleanAt
{
	const ProllyHash *root;
	Snapshot	snap;
	char	   *name;
	Oid			branch;
	ProllyNodeRef baseline;
	BranchFoldStamp at;			/* what it was known at */
} CleanAt;

static void
find_clean_at(const char *name, Oid branch, void *arg)
{
	CleanAt    *ca = arg;
	ProllyNodeRef baseline;
	ProllyHash	h;

	if (ca->name != NULL || branch == MyBranch ||
		!branch_baseline(name, &baseline))
		return;
	if (branch_fold_known_at(branch, ca->snap, &baseline.hash, false, &h,
							 &ca->at) &&
		same_hash(&h, ca->root))
	{
		ca->name = pstrdup(name);
		ca->branch = branch;
		ca->baseline = baseline;
	}
}

/*
 * Is ca's branch still at its root, as it was known to be?  Asked of what
 * was known then: the first table shared changes pg_class, after which
 * nothing new is known in the transaction, though nothing about the other
 * branch changed.
 */
static bool
still_clean(void *arg)
{
	CleanAt    *ca = arg;

	return branch_fold_unchanged(&ca->at);
}

/* A root's table, as tables() visits it. */
typedef struct NamedTree
{
	char	   *name;
	ProllyNodeRef data;
	bool		readable;
} NamedTree;

static void
note_table(const char *name, const ProllyNodeRef *data,
		   const ProllyValue *description, const char *unreadable, void *arg)
{
	NamedTree  *t = palloc(sizeof(NamedTree));

	t->name = pstrdup(name);
	t->data = *data;
	t->readable = unreadable == NULL;
	*(List **) arg = lappend(*(List **) arg, t);
}

static void
count_change(ProllyDiffKind kind, const ProllyKey *key,
			 const ProllyValue *before, const ProllyValue *after, void *arg)
{
	(*(uint64 *) arg)++;
}

/*
 * Prolly table relid and the relations stored with it: its indexes, its
 * TOAST table and change log, and theirs; their blocks counted.
 */
static List *
stored_with(Oid relid, uint64 *blocks)
{
	List	   *relids = list_make1_oid(relid);
	ProllyComponents c;

	*blocks = 0;
	for (int i = 0; i < list_length(relids); i++)
	{
		Relation	rel = relation_open(list_nth_oid(relids, i), AccessShareLock);

		*blocks += RelationGetNumberOfBlocks(rel);
		if (rel->rd_rel->relkind != RELKIND_INDEX)
			relids = list_concat(relids, RelationGetIndexList(rel));
		if (OidIsValid(rel->rd_rel->reltoastrelid))
			relids = lappend_oid(relids, rel->rd_rel->reltoastrelid);
		if (i == 0)
		{
			prolly_components_for_relation(rel, &c);
			if (c.change_log && c.change_log->relid != NULL &&
				OidIsValid(c.change_log->relid(relid)))
				relids = lappend_oid(relids, c.change_log->relid(relid));
		}
		relation_close(rel, NoLock);
	}
	return relids;
}

/*
 * The tables of the session's branch whose rows differ from target's made
 * the copies another branch has where that costs less (true: some were);
 * *working is then the branch's working set, folded as its baseline.
 */
static bool
share_clean_tables(const ProllyNodeRef *target, ProllyNodeRef *working)
{
	ProllyDatabaseComponents dc;
	CleanAt		ca = {0};
	List	   *ours = NIL;
	List	   *theirs = NIL;
	List	   *chosen = NIL;	/* NamedTree *, then each one's relations */
	List	   *relations = NIL;
	List	   *shared = NIL;
	ProllyTreeEnv denv;
	ProllyNodeRef baseline;
	ProllyNodeRef spliced;
	ProllyNodeRef next;
	ListCell   *a;
	ListCell   *b;
	char	   *current = branch_ops()->current();

	prolly_database_components(&dc);
	if (branch_ops()->share == NULL || dc.canonicalizer->splice_baseline == NULL ||
		!fold_caches_usable() || !branch_baseline(current, &baseline))
		return false;
	ca.root = &target->hash;
	ca.snap = fold_snapshot();
	branch_ops()->branches(find_clean_at, &ca);
	if (ca.name == NULL ||
		!PROLLY_CALL(dc.canonicalizer, same_schema, working, target))
		return false;

	/* the tables whose rows differ, where sharing costs less */
	PROLLY_CALL(dc.canonicalizer, tables, working, NULL, note_table, &ours);
	PROLLY_CALL(dc.canonicalizer, tables, target, NULL, note_table, &theirs);
	if (list_length(ours) != list_length(theirs))
		return false;
	prolly_folded_tree_env(&denv);
	forboth(a, ours, b, theirs)
	{
		NamedTree  *o = lfirst(a);
		NamedTree  *t = lfirst(b);
		uint64		changes = 0;
		uint64		blocks;
		Oid			relid;
		List	   *relids;

		if (strcmp(o->name, t->name) != 0)
			return false;
		if (same_hash(&o->data.hash, &t->data.hash) || !o->readable ||
			!t->readable)
			continue;
		relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(o->name, NULL)),
								 NoLock, true);
		if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION)
			continue;
		relids = stored_with(relid, &blocks);
		PROLLY_CALL(tree, diff, &denv, &o->data, &t->data, count_change,
					&changes);
		if ((double) changes * MOVE_ROW_USEC <= (double) blocks * SHARE_BLOCK_USEC)
			continue;
		chosen = lappend(chosen, o);
		relations = lappend(relations, relids);
	}
	if (chosen == NIL)
		return false;
	if (debug_prolly_verify_fold_caches)
		verify_known(ca.name, false, target);

	/*
	 * (each shared if nobody uses it now, and the other branch is still at
	 * the target with it locked: none of its writers ended since, nor can
	 * write it now)
	 */
	forboth(a, chosen, b, relations)
	{
		if (branch_ops()->share(ca.branch, lfirst(b), still_clean, &ca))
			shared = lappend(shared, ((NamedTree *) lfirst(a))->name);
	}
	if (shared == NIL)
		return false;
	PROLLY_CALL(dc.canonicalizer, splice_baseline, &baseline, &ca.baseline,
				shared, &spliced);
	branch_set_baseline(current, &spliced, NULL);
	fold_from_baseline(working, &next, false);
	branch_set_baseline(current, &next, working);
	return true;
}

/*
 * After a move of rows from working to target: the session branch's
 * baseline (which holds working's rows, as the fold before the move left
 * it) advanced to target without a fold, each table whose rows the move
 * wrote holding target's, its change finder's record of the move's own
 * writes consumed (the canonicalizer's advance_baseline()), its fold's
 * working set recorded as target (#482).  (Not published as known: the
 * move wrote the branch in this transaction, which moves its generation as
 * it ends; the next read folds, finding nothing left to read.)  False,
 * the caller folding, where
 * a table's can't be so: the definitions or a materialized view's rows
 * moved too, or a finder can't advance a table's state (then those that
 * were advanced stay so, and the fold reads only the rest).
 */
static bool
advance_to_target(const ProllyNodeRef *target, const ProllyNodeRef *working)
{
	ProllyDatabaseComponents dc;
	char	   *name = branch_ops()->current();
	ProllyNodeRef baseline;
	ProllyNodeRef next;
	List	   *ours = NIL;
	List	   *theirs = NIL;
	ListCell   *a;
	ListCell   *b;
	bool		all = true;
	bool		any = false;

	prolly_database_components(&dc);
	if (!fold_caches_usable() ||
		!PROLLY_HAS(dc.canonicalizer, advance_baseline) ||
		!branch_baseline(name, &baseline) ||
		!PROLLY_CALL(dc.canonicalizer, same_schema, working, target))
		return false;
	PROLLY_CALL(dc.canonicalizer, materialized_views, working, note_table,
				&ours);
	PROLLY_CALL(dc.canonicalizer, materialized_views, target, note_table,
				&theirs);
	if (list_length(ours) != list_length(theirs))
		return false;
	forboth(a, ours, b, theirs)
	{
		NamedTree  *o = lfirst(a);
		NamedTree  *t = lfirst(b);

		if (strcmp(o->name, t->name) != 0 ||
			!same_hash(&o->data.hash, &t->data.hash))
			return false;
	}
	ours = theirs = NIL;
	PROLLY_CALL(dc.canonicalizer, tables, working, NULL, note_table, &ours);
	PROLLY_CALL(dc.canonicalizer, tables, target, NULL, note_table, &theirs);
	if (list_length(ours) != list_length(theirs))
		return false;
	forboth(a, ours, b, theirs)
	{
		if (strcmp(((NamedTree *) lfirst(a))->name,
				   ((NamedTree *) lfirst(b))->name) != 0)
			return false;
	}
	forboth(a, ours, b, theirs)
	{
		NamedTree  *o = lfirst(a);
		NamedTree  *t = lfirst(b);
		ProllyRowChanges ch = {InvalidOid, NULL, 0};

		if (same_hash(&o->data.hash, &t->data.hash))
			continue;
		ch.relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(o->name, NULL)),
									NoLock, true);
		if (OidIsValid(ch.relid) && get_rel_relkind(ch.relid) == RELKIND_RELATION &&
			PROLLY_CALL(dc.canonicalizer, advance_baseline, &baseline, working,
						target, &ch, 1, &next))
		{
			baseline = next;
			any = true;
		}
		else
			all = false;
	}
	if (!all)
	{
		if (any)
			branch_set_baseline(name, &baseline, NULL);
		return false;
	}
	branch_set_baseline(name, &baseline, target);
	if (debug_prolly_verify_fold_caches)
		verify_known(name, false, target);
	return true;
}

static void
move_settled(void *arg)
{
	settle(arg, NULL);
}

static bool copies_commit_get(const ProllyHash *id, ProllyCommitInfo *info);
static bool commit_time(const ProllyHash *id, TimestampTz *at);
static bool commit_height(const ProllyHash *id, int64 *height);

/* The session's branch folded, as a commit's fold would be: its baseline. */
static void
fold_as_baseline(void *arg)
{
	ProllyNodeRef root;
	ProllyNodeRef next;

	fold_from_baseline(&root, &next, false);
	branch_set_baseline(branch_ops()->current(), &next, &root);
}

/*
 * Has a transaction the snapshot doesn't see (running when it was taken,
 * or begun since) committed?  Only one such could have written what a
 * copy holds and the snapshot doesn't see.  True too if too many began
 * since to ask each.
 */
#define UNSEEN_XIDS_ASKED	65536

static bool
commits_unseen(Snapshot snap)
{
	TransactionId next = XidFromFullTransactionId(ReadNextFullTransactionId());
	TransactionId xid;

	for (uint32 i = 0; i < snap->xcnt; i++)
		if (TransactionIdDidCommit(snap->xip[i]))
			return true;
	if ((uint32) (next - snap->xmax) > UNSEEN_XIDS_ASKED)
		return true;
	for (xid = snap->xmax; TransactionIdPrecedes(xid, next);)
	{
		if (!TransactionIdIsCurrentTransactionId(xid) &&
			TransactionIdDidCommit(xid))
			return true;
		TransactionIdAdvance(xid);
	}
	return false;
}

/* Did a commit the snapshot doesn't see write xid's part of a tuple? */
static bool
xid_unseen(TransactionId xid, Snapshot snap)
{
	return TransactionIdIsNormal(xid) &&
		!TransactionIdIsCurrentTransactionId(xid) &&
		XidInMVCCSnapshot(xid, snap) && TransactionIdDidCommit(xid);
}

/*
 * Does relation rel hold a tuple a commit the snapshot doesn't see
 * inserted or deleted?  (Every page is read.)
 */
static bool
holds_unseen(Relation rel, Snapshot snap)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	bool		found = false;

	for (BlockNumber blk = 0; blk < nblocks && !found; blk++)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, strategy);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		maxoff = PageIsNew(page) ? InvalidOffsetNumber :
			PageGetMaxOffsetNumber(page);
		for (OffsetNumber off = FirstOffsetNumber; off <= maxoff && !found;
			 off = OffsetNumberNext(off))
		{
			ItemId		lp = PageGetItemId(page, off);
			HeapTupleHeader t;
			uint16		mask;

			if (!ItemIdIsNormal(lp))
				continue;
			t = (HeapTupleHeader) PageGetItem(page, lp);
			mask = t->t_infomask;
			if (!HeapTupleHeaderXminFrozen(t) && !(mask & HEAP_XMIN_INVALID) &&
				xid_unseen(HeapTupleHeaderGetRawXmin(t), snap))
				found = true;
			else if (!(mask & HEAP_XMAX_INVALID) &&
					 !HEAP_XMAX_IS_LOCKED_ONLY(mask) &&
					 xid_unseen(HeapTupleHeaderGetUpdateXid(t), snap))
				found = true;
		}
		UnlockReleaseBuffer(buf);
	}
	FreeAccessStrategy(strategy);
	return found;
}

/*
 * Do the copies of the branch being made hold what the active snapshot
 * doesn't see: rows a transaction it doesn't see committed, or the old
 * versions of rows such a one changed?  Only a transaction that committed
 * after the snapshot could have written them; if one did, a table holds
 * them only if that one wrote it.  A table's change log tells which wrote
 * it without reading the table (its entries are written with each write,
 * and kept while the snapshot is); one without a log has every page read.
 * Asked only once commits_unseen() says one did.
 */
static bool
copies_hold_unseen(void)
{
	Snapshot	snap = GetActiveSnapshot();
	Relation	classes;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *relids = NIL;
	bool		found = false;

	classes = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(classes, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relam == PROLLY_TABLE_AM_OID &&
			(c->relkind == RELKIND_RELATION || c->relkind == RELKIND_MATVIEW))
			relids = lappend_oid(relids, c->oid);
	}
	systable_endscan(scan);
	table_close(classes, AccessShareLock);

	foreach_oid(relid, relids)
	{
		Relation	rel = table_open(relid, AccessShareLock);
		ProllyComponents c;

		prolly_components_for_relation(rel, &c);
		if (c.change_log && PROLLY_HAS(c.change_log, written_unseen) &&
			PROLLY_CALL(c.change_log, identity, rel) != 0)
			found = PROLLY_CALL(c.change_log, written_unseen, rel, snap);
		else
		{
			pgstat_count_prolly(PGSTAT_PROLLY_FIRST_USE_TABLES_SCANNED, 1);
			found = holds_unseen(rel, snap);
		}
		table_close(rel, AccessShareLock);
		if (found)
			break;
	}
	list_free(relids);
	return found;
}

/* A first use's move: to target, from copies of source's (NULL: none). */
typedef struct MoveCopies
{
	const ProllyNodeRef *target;
	const KnownSource *source;
} MoveCopies;

/*
 * A branch's first use's move to its head.  The copy waited for the
 * source branch's writers, so its pages hold every row they committed,
 * some after the active snapshot was taken: rows it doesn't see, and the
 * old versions of rows it sees that they deleted or updated.  A fold by
 * that snapshot wouldn't see the former, and a move by it wouldn't undo
 * them; the latter it would see still, while every later snapshot
 * wouldn't.  Read committed, the move goes by a newer snapshot, which sees
 * everything the copies hold.  A transaction whose snapshot can't move
 * (repeatable read, serializable) fails, to be retried, if the copies hold
 * such rows (as when it can't see the head it would commit on): what it
 * saw of the branch would otherwise not be what everyone sees once it
 * commits.  Commits elsewhere (another database, tables the branch's copies
 * don't hold) don't stop it.
 */
static void
move_copies(void *arg)
{
	MoveCopies *mc = arg;

	if (IsolationUsesXactSnapshot())
	{
		/*
		 * (with a commit the snapshot doesn't see, the source's known
		 * working set may be newer than the snapshot: folded from scratch)
		 */
		if (commits_unseen(GetActiveSnapshot()))
		{
			if (copies_hold_unseen())
			{
				pgstat_count_prolly(PGSTAT_PROLLY_FIRST_USE_REFUSALS, 1);
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access: the branch's first use copies rows committed after this transaction's snapshot"),
						 errhint("The transaction might succeed if retried.")));
			}
			settle(mc->target, NULL);
		}
		else
			settle(mc->target, mc->source);
		return;
	}
	PushActiveSnapshot(GetLatestSnapshot());
	settle(mc->target, mc->source);
	PopActiveSnapshot();
}

/*
 * The branch with copies nearest a root (#491), ranked: its working set
 * known to be the root (3: copied, nothing is written or folded, if it
 * is still known once copied: known, what it was known at), its
 * baseline's fold or its head made the root (2, 1: only what it changed
 * since is).  The session's branch on a tie; none (rank 0) if no branch
 * is at the root.
 */
typedef struct NearestSource
{
	const ProllyHash *root;
	Snapshot	snap;
	char	   *name;
	Oid			branch;
	int			rank;
	KnownSource known;
} NearestSource;

static void
rank_source(const char *name, Oid branch, void *arg)
{
	NearestSource *ns = arg;
	ProllyNodeRef baseline;
	ProllyNodeRef ref;
	ProllyHash	h;
	ProllyCommitInfo info;
	BranchFoldStamp at;
	int			rank = 0;

	if (ns->rank == 3 && ns->branch == MyBranch)
		return;
	if (fold_caches_usable() &&
		branch_fold_known_at(branch, ns->snap,
							 branch_baseline(name, &baseline) ? &baseline.hash : NULL,
							 false, &h, &at) &&
		same_hash(&h, ns->root))
		rank = 3;
	else if (branch_baseline_root(name, &ref) && same_hash(&ref.hash, ns->root))
		rank = 2;
	else if (branch_head(name, &h) && copies_commit_get(&h, &info) &&
			 same_hash(&info.root.hash, ns->root))
		rank = 1;
	if (rank > ns->rank || (rank > 0 && rank == ns->rank && branch == MyBranch))
	{
		ns->rank = rank;
		ns->name = pstrdup(name);
		ns->branch = branch;
		if (rank == 3)
		{
			ns->known.name = ns->name;
			ns->known.root = h;
			ns->known.at = at;
		}
	}
}

/*
 * Make the copies of a branch that has only its head: a copy of the
 * nearest branch's to its head's root (#491), or else the session's (each
 * has the same changes since the same baseline as the copy), moved to the
 * root.
 */
static void
materialize(const char *name)
{
	ProllyHash	head;
	ProllyHash	checked;
	ProllyCommitInfo info;
	ProllyNodeRef baseline;
	bool		was_read_only = XactReadOnly;
	int			hush;
	NearestSource ns = {0};
	MoveCopies	mc;
	char	   *source;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("branch \"%s\" hasn't been used yet", name),
				 errdetail("A branch's first use makes its copies, which a standby can't."),
				 errhint("Use it on the primary first.")));

	/*
	 * What its head needs from outside, checked before the refs are held
	 * (the check reads catalogs and extensions' control files; other
	 * sessions' commits needn't wait for it), and again under them only if
	 * the head moved meanwhile.
	 */
	if (branch_head(name, &checked) && copies_commit_get(&checked, &info))
		prolly_check_commit_dependencies(&info, NULL, "branch's first use", false);
	else
		memset(&checked, 0, sizeof(checked));

	/*
	 * Its copies are the branch's as much as its head is: made whatever the
	 * session may make or write, as it is.
	 */
	XactReadOnly = false;
	hush = branch_writes_hush();
	PG_TRY();
	{
		/* (with the refs held: no other session makes or deletes it meanwhile) */
		lock_refs();
		AcceptInvalidationMessages();
		if (OidIsValid(branch_oid(name)) || !branch_head(name, &head))
			goto done;
		if (!copies_commit_get(&head, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the head commit of branch \"%s\" is missing", name)));
		if (!same_hash(&head, &checked))
			prolly_check_commit_dependencies(&info, NULL, "branch's first use", false);
		ns.root = &info.root.hash;
		ns.snap = IsolationUsesXactSnapshot() ? GetActiveSnapshot() :
			GetLatestSnapshot();
		branch_ops()->branches(rank_source, &ns);
		source = ns.rank > 0 ? ns.name : branch_ops()->current();
		branch_ops()->make(name, ns.rank > 0 ? source : NULL);
		branch_set_baseline(name,
							branch_baseline(source, &baseline) ? &baseline : NULL,
							NULL);

		/*
		 * (moved to the head, unless the copies are there already: those of
		 * a branch whose working set is, if it still is)
		 */
		mc.target = &info.root;
		mc.source = ns.rank == 3 ? &ns.known : NULL;
		branch_ops()->run_on(name, move_copies, &mc);
done:
		;
	}
	PG_FINALLY();
	{
		branch_writes_unhush(hush);
		XactReadOnly = was_read_only;
	}
	PG_END_TRY();
}

static void
note_made(const char *name, Oid branch, void *arg)
{
	(void) hash_search((HTAB *) arg, name, HASH_ENTER, NULL);
}

/* Visit the branches that have only a head (not yet copies). */
static void
visit_head_only(void (*visit) (const char *name, void *arg), void *arg)
{
	ProllyNodeRef root;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	ProllyKey	start = {(const uint8 *) "B", 1};
	List	   *names = NIL;
	ListCell   *lc;

	if (!refs_root(&root))
		return;
	c = PROLLY_CALL(tree, cursor_open, &env, &root, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, &start, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		if (k.len < 1 || k.data[0] != TAG_BRANCH)
			break;
		names = lappend(names, pnstrdup((const char *) k.data + 1, k.len - 1));
	}
	PROLLY_CALL(tree, cursor_close, c);
	if (names == NIL)
		return;

	/* (the branches with copies, read once) */
	{
		HASHCTL		ctl;
		HTAB	   *made;

		ctl.keysize = NAMEDATALEN;
		ctl.entrysize = NAMEDATALEN;
		ctl.hcxt = CurrentMemoryContext;
		made = hash_create("prolly branches with copies", 64, &ctl,
						   HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
		branch_ops()->branches(note_made, made);
		foreach(lc, names)
		{
			if (strlen(lfirst(lc)) >= NAMEDATALEN ||
				hash_search(made, lfirst(lc), HASH_FIND, NULL) == NULL)
				visit(lfirst(lc), arg);
		}
		hash_destroy(made);
	}
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

static bool copies_lookup_commit(const ProllyHash *id, ProllyCommitInfo *info,
								 void *arg);

/* 8 bytes big-endian, as a commit's "height" and "time" items are. */
static void
put_uint64(StringInfo v, uint64 n)
{
	for (int i = 7; i >= 0; i--)
		appendStringInfoChar(v, (char) (n >> (i * 8)));
}

/*
 * The height of a commit with these parents: 1 + the most of theirs, each
 * read alone; one that records none has its found from its history, once.
 */
static int64
parents_height(const ProllyHash *parents, int nparents)
{
	MemoryContext cxt = NULL;
	MemoryContext old = CurrentMemoryContext;
	HTAB	   *heights = NULL;
	int64		height = 1;

	for (int i = 0; i < nparents; i++)
	{
		int64		parent;

		if (!commit_height(&parents[i], &parent))
		{
			if (cxt == NULL)
			{
				cxt = AllocSetContextCreate(CurrentMemoryContext,
											"prolly commit height",
											ALLOCSET_DEFAULT_SIZES);
				MemoryContextSwitchTo(cxt);
				heights = prolly_history_heights();
			}
			parent = prolly_history_height(&parents[i], heights,
										   copies_lookup_commit, NULL);
		}
		height = Max(height, parent + 1);
	}
	MemoryContextSwitchTo(old);
	if (cxt)
		MemoryContextDelete(cxt);
	return height;
}

/*
 * Write a commit's tree; its id is the root's hash.  What its versioned
 * objects need from outside is folded now, from the catalogs as the
 * transaction sees them.  It is dated now, as Dolt's are, not at the
 * transaction's start, so a parent another session committed since then is
 * older than it; and never at or before a parent's date (the clock may have
 * stepped back, or a parent come from a remote whose clock runs ahead), so
 * along history the dates only rise.  (A parent dated outside timestamp's
 * range, or at its last microsecond, which only a foreign or corrupt commit
 * could be, leaves the date as it is.)  A date the commit was given
 * (dolt_commit --date) is its date, as Dolt's is.
 */
static void
write_commit(const ProllyHash *root, const ProllyHash *parents, int nparents,
			 const char *author, const char *message, ProllyHash *id)
{
	TimestampTz at = GetCurrentTimestamp();
	int			nparents_dated = nparents;
	ProllyTreeBuilder *b;
	StringInfoData v;
	ProllyNodeRef ref;
	ProllyNodeRef deps;
	ProllyDatabaseComponents dc;

	/* (a date given, as dolt_commit --date: as given, whatever the parents') */
	if (prolly_commit_date(&at))
		nparents_dated = 0;
	for (int i = 0; i < nparents_dated; i++)
	{
		TimestampTz parent_at;

		if (commit_time(&parents[i], &parent_at) && at <= parent_at &&
			IS_VALID_TIMESTAMP(parent_at) && IS_VALID_TIMESTAMP(parent_at + 1))
			at = parent_at + 1;
	}
	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, fold_dependencies, &deps);
	b = PROLLY_CALL(tree, build_begin, &commit_env, CurrentMemoryContext);

	/*
	 * Keys in order: author, dependencies, height, message, parents, root,
	 * time.
	 */
	initStringInfo(&v);
	appendStringInfoString(&v, author);
	add_item(b, "author", &v);
	resetStringInfo(&v);
	put_hash(&v, &deps.hash);
	add_item(b, "dependencies", &v);
	resetStringInfo(&v);
	put_uint64(&v, (uint64) parents_height(parents, nparents));
	add_item(b, "height", &v);
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
	put_uint64(&v, (uint64) at);
	add_item(b, "time", &v);
	PROLLY_CALL(tree, build_finish, b, &ref);
	*id = ref.hash;
	pfree(v.data);
	pgstat_count_prolly(PGSTAT_PROLLY_COMMITS, 1);
}

static bool
read_item(const ProllyNodeRef *commit, const char *key, StringInfo out)
{
	ProllyKey	k = {(const uint8 *) key, strlen(key)};

	return PROLLY_CALL(tree, get, &env, commit, &k, out);
}

/* A commit's 8-byte item, as put_uint64() puts it; false if it isn't one. */
static bool
get_uint64(const StringInfoData *v, uint64 *n)
{
	uint64		t = 0;

	if (v->len != 8)
		return false;
	for (int i = 0; i < 8; i++)
		t = (t << 8) | (uint8) v->data[i];
	*n = t;
	return true;
}

/* A commit's "time" item, as write_commit puts it; false if it isn't one. */
static bool
get_time(const StringInfoData *v, TimestampTz *at)
{
	uint64		t;

	if (!get_uint64(v, &t))
		return false;
	*at = (TimestampTz) t;
	return true;
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
	if (read_item(&ref, "dependencies", &v))
	{
		ProllyHash	deps;

		(void) get_hash(v.data, v.data + v.len, &deps);
		ref_of(&deps, &info->dependencies);
		info->has_dependencies = true;
	}
	resetStringInfo(&v);
	if (read_item(&ref, "height", &v))
	{
		uint64		height;

		if (get_uint64(&v, &height) && height > 0 && height <= PG_INT64_MAX)
		{
			info->height = (int64) height;
			info->has_height = true;
		}
	}
	resetStringInfo(&v);
	if (read_item(&ref, "time", &v))
		(void) get_time(&v, &info->committed_at);
	pfree(v.data);
	pgstat_count_prolly(PGSTAT_PROLLY_COMMITS_READ, 1);
	return true;
}

/* Just the date of commit id, which is read only for it. */
static bool
commit_time(const ProllyHash *id, TimestampTz *at)
{
	ProllyNodeRef ref;
	StringInfoData v;
	bool		found;

	refs_init();
	if (id->len == 0 || !PROLLY_CALL(env.chunk_store, has, env.chunks, id))
		return false;
	ref_of(id, &ref);
	initStringInfo(&v);
	found = read_item(&ref, "time", &v) && get_time(&v, at);
	pfree(v.data);
	return found;
}

/* Just the height commit id records, which is read only for it. */
static bool
commit_height(const ProllyHash *id, int64 *height)
{
	ProllyNodeRef ref;
	StringInfoData v;
	uint64		h;
	bool		found;

	refs_init();
	if (id->len == 0 || !PROLLY_CALL(env.chunk_store, has, env.chunks, id))
		return false;
	ref_of(id, &ref);
	initStringInfo(&v);
	found = read_item(&ref, "height", &v) && get_uint64(&v, &h) &&
		h > 0 && h <= PG_INT64_MAX;
	pfree(v.data);
	if (found)
		*height = (int64) h;
	return found;
}

/* A commit id in hex; false if refname isn't one. */
static bool
parse_commit_id(const char *refname, ProllyHash *id)
{
	return prolly_hash_from_hex(refname, strlen(refname), id);
}

static bool
resolve_base(const char *refname, ProllyHash *commit_id)
{
	ProllyCommitInfo info;

	/*
	 * HEAD, in any case: the session branch's head commit, as in Dolt.  (A
	 * branch so named, made before HEAD's other cases were refused, is
	 * still itself.)
	 */
	if (pg_strcasecmp(refname, "HEAD") == 0 && !branch_exists(refname))
		refname = branch_ops()->current();
	/*
	 * a branch by its full ref name, as in Dolt and git (unless a branch
	 * is named so itself)
	 */
	else if (strncmp(refname, "refs/heads/", 11) == 0 && refname[11] != '\0' &&
			 !branch_exists(refname))
		refname += 11;
	if (branch_head(refname, commit_id) || OidIsValid(branch_oid(refname)))
		return commit_id->len > 0;
	if (tracking_head(refname, commit_id))
		return true;
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

/* The best common ancestor of a and b (prolly_history_merge_base()). */
static bool
copies_merge_base(const ProllyHash *a, const ProllyHash *b, ProllyHash *base)
{
	refs_init();
	return prolly_history_merge_base(a, b, copies_lookup_commit, NULL, base);
}

/* The snapshot a fold reads by: the canonicalizer's. */
static Snapshot
fold_snapshot(void)
{
	return ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
}

/* May a working set be taken from an earlier fold?  Not one forced to read. */
static bool
fold_caches_usable(void)
{
	return !debug_prolly_force_full_fold && !prolly_fold_verifying;
}

/*
 * Publish the fold of the session's branch stamped, by snap, made from
 * baseline (NULL: none) (#482): complete if it left nothing out.
 */
static void
publish_fold(const BranchFoldStamp *stamp, Snapshot snap,
			 const ProllyNodeRef *baseline, const ProllyNodeRef *root,
			 bool complete)
{
	if (fold_caches_usable())
		branch_fold_publish(stamp, snap, baseline ? &baseline->hash : NULL,
							&root->hash, complete);
}

static void verify_known(const char *name, bool complete,
						 const ProllyNodeRef *root);

/*
 * The working set of branch name (OID branch), from baseline (NULL: none),
 * taken from an earlier fold if nothing it read changed since (#482):
 * false if not known.  complete: only one that left nothing out.
 */
static bool
known_working_set(const char *name, Oid branch, const ProllyNodeRef *baseline,
				  bool complete, ProllyNodeRef *root)
{
	ProllyHash	h;

	if (!fold_caches_usable() ||
		!branch_fold_known(branch, fold_snapshot(),
						   baseline ? &baseline->hash : NULL, complete, &h))
		return false;
	ref_of(&h, root);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLDS_REUSED, 1);
	if (debug_prolly_verify_fold_caches)
		verify_known(name, complete, root);
	return true;
}

/*
 * The session branch's versioned schemas, folded from its baseline; with
 * next, a new baseline begun.  Only a commit's fold refuses objects the
 * canonical form doesn't describe; others leave them out.  Published, from
 * next if made (#482).
 */
static void
fold_from_baseline(ProllyNodeRef *root, ProllyNodeRef *next, bool commit)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef baseline;
	bool		have = branch_baseline(branch_ops()->current(), &baseline);
	Snapshot	snap = fold_snapshot();
	BranchFoldStamp stamp;

	branch_fold_stamp(MyBranch, &stamp);
	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, fold, have ? &baseline : NULL, root, next,
				commit);

	/* (a commit's fold refuses what it would leave out: it left nothing) */
	publish_fold(&stamp, snap, next ? next : (have ? &baseline : NULL), root,
				 commit);
}

static void
fold(ProllyNodeRef *root)
{
	fold_from_baseline(root, NULL, false);
}

/* fold()'s working set, taken from an earlier fold if known (#482). */
static void
working_fold(ProllyNodeRef *root)
{
	ProllyNodeRef baseline;
	char	   *name = branch_ops()->current();
	bool		have = branch_baseline(name, &baseline);

	if (!known_working_set(name, MyBranch, have ? &baseline : NULL, false,
						   root))
		fold(root);
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

	lock_branch(name);
	if (nparents > 0 && branch_head(name, &head) && !same_hash(&head, &parents[0]))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access: branch \"%s\" moved while this was being committed",
						name),
				 errhint("The transaction might succeed if retried.")));
	return see_head(name);
}

static int
line_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(lfirst(a), lfirst(b));
}

/*
 * The session's prolly tables, each "relid name" on a line, by relid: as
 * the branch's catalog names them now.
 */
static void
table_names(StringInfo out)
{
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tup;
	List	   *lines = NIL;
	ListCell   *lc;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relam == PROLLY_TABLE_AM_OID && c->relkind == RELKIND_RELATION)
			lines = lappend(lines, psprintf("%010u %s.%s", c->oid,
											get_namespace_name(c->relnamespace),
											NameStr(c->relname)));
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	list_sort(lines, line_cmp);
	foreach(lc, lines)
	{
		appendStringInfoString(out, lfirst(lc));
		appendStringInfoChar(out, '\n');
	}
}

/*
 * The tables of the branch's head, as named by the commit that made it of
 * the working set: the head's hash on the first line, then table_names().
 * A commit of a transaction's own rows can tell by it a table renamed since
 * (by a writer whose change no commit holds yet) from the one the head has
 * under that name.
 */
static void
names_key(StringInfo key, const char *name)
{
	appendStringInfoChar(key, TAG_NAMES);
	appendStringInfoString(key, name);
}

static void
note_head_names(const char *name, const ProllyHash *head)
{
	StringInfoData key;
	StringInfoData v;
	char	   *hex = prolly_hash_hex(head);

	initStringInfo(&key);
	initStringInfo(&v);
	names_key(&key, name);
	appendStringInfo(&v, "%s\n", hex);
	table_names(&v);
	refs_put(key.data, key.len, &v);
	pfree(hex);
	pfree(key.data);
	pfree(v.data);
}

/*
 * The head names its tables as the record says, if the record is of head:
 * their names in it, from the first line on (with it, after the hash).
 */
static char *
head_names(const char *name, const ProllyHash *head)
{
	StringInfoData key;
	StringInfoData v;
	char	   *hex = prolly_hash_hex(head);
	char	   *lines = NULL;

	initStringInfo(&key);
	initStringInfo(&v);
	names_key(&key, name);
	if (refs_get(key.data, key.len, &v) &&
		strncmp(v.data, hex, strlen(hex)) == 0 && v.data[strlen(hex)] == '\n')
		lines = pstrdup(v.data + strlen(hex));
	pfree(hex);
	pfree(key.data);
	pfree(v.data);
	return lines;
}

/* The record of head, now of next (a commit that renamed nothing). */
static void
carry_head_names(const char *name, const ProllyHash *head, const ProllyHash *next)
{
	char	   *lines = head_names(name, head);
	StringInfoData key;
	StringInfoData v;
	char	   *hex;

	if (lines == NULL)
		return;
	hex = prolly_hash_hex(next);
	initStringInfo(&key);
	initStringInfo(&v);
	names_key(&key, name);
	appendStringInfo(&v, "%s%s", hex, lines);
	refs_put(key.data, key.len, &v);
	pfree(hex);
	pfree(lines);
	pfree(key.data);
	pfree(v.data);
}

/* Are rel's name now and its name at head the same? */
static bool
named_as_at_head(const char *lines, Relation rel)
{
	char	   *line = psprintf("\n%010u %s.%s\n", RelationGetRelid(rel),
								get_namespace_name(RelationGetNamespace(rel)),
								RelationGetRelationName(rel));
	bool		same = strstr(lines, line) != NULL;

	pfree(line);
	return same;
}

/*
 * Commit the session branch as it is, with these parents; the fold is the
 * branch's next baseline.  Unless allow_empty, a commit of the branch's
 * head alone whose root is the head's makes no commit and returns false
 * (what its objects need from outside isn't a change: an extension's
 * update alone isn't committed); the fold still advances the baseline, as
 * a checkpoint's would.
 */
static bool
commit_branch(const char *message, const ProllyHash *parents, int nparents,
			  bool allow_empty, ProllyHash *commit_id)
{
	char	   *name = branch_ops()->current();
	ProllyNodeRef root;
	ProllyNodeRef next;
	ProllyNodeRef parent;
	ProllyHash	head;
	bool		pushed;

	pushed = hold_for_commit(name, parents, nparents);
	fold_from_baseline(&root, &next, true);
	if (pushed)
		PopActiveSnapshot();

	/* (the one parent is the head: a merge's first commit isn't empty) */
	if (!allow_empty && nparents == 1 &&
		branch_head(name, &head) && same_hash(&head, &parents[0]) &&
		copies_commit_working_set(&parents[0], &parent) &&
		same_hash(&parent.hash, &root.hash))
	{
		branch_set_baseline(name, &next, &root);
		return false;
	}
	write_commit(&root.hash, parents, nparents, prolly_commit_author(),
				 message, commit_id);
	branch_set_head(name, commit_id);
	branch_set_baseline(name, &next, &root);
	note_head_names(name, commit_id);
	return true;
}

/* ------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------
 */

/*
 * A branch whose name differs from name only by case, other than ignore:
 * found into *arg.
 */
typedef struct OtherCase
{
	const char *name;
	const char *ignore;
	char	   *found;
} OtherCase;

static void
find_other_case(const char *name, const ProllyHash *head, void *arg)
{
	OtherCase  *oc = arg;

	if (oc->found == NULL && strcmp(name, oc->name) != 0 &&
		(oc->ignore == NULL || strcmp(name, oc->ignore) != 0) &&
		pg_strcasecmp(name, oc->name) == 0)
		oc->found = pstrdup(name);
}

static void copies_branches(ProllyBranchVisit visit, void *arg);

/*
 * Refuse a new branch name that differs from a branch's, other than
 * ignore's (a branch being renamed), only by case, as Dolt does: on a
 * case-insensitive file system (or to a user) they're one.  Names are
 * ASCII, folded as such.  (The hidden branches of sessions checked out at
 * commits, named for them, aren't checked: no user names them.)  Every
 * branch is visited: a create's cost grows with the branches there are.
 */
static void
refuse_other_case(const char *name, const char *ignore)
{
	OtherCase	oc = {name, ignore, NULL};

	if (strncmp(name, PROLLY_DETACHED_PREFIX, strlen(PROLLY_DETACHED_PREFIX)) == 0)
		return;
	copies_branches(find_other_case, &oc);
	if (oc.found)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" already exists", oc.found),
				 errdetail("Branch names that differ only by case name one branch.")));
}

/*
 * Refuse a new branch name that is a commit's id (in any case, as ids are
 * read): a branch so named would hide the commit wherever refs are read,
 * branches being looked for first.  Checked before the name's other
 * rules, whatever the length of the ids the hash makes.
 */
static void
refuse_commit_name(const char *name)
{
	ProllyHash	id;
	ProllyCommitInfo info;

	if (parse_commit_id(name, &id) && copies_commit_get(&id, &info))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid branch name \"%s\"", name),
				 errdetail("It is a commit's id.")));
}

static void
copies_branch_create(const char *name, const char *start_point)
{
	const char *from = start_point ? start_point : branch_ops()->current();
	ProllyHash	id;
	ProllyCommitInfo info;
	bool		from_exists;

	refs_init();
	refuse_commit_name(name);
	branch_ops()->check_create(name);
	lock_refs();
	refuse_other_case(name, NULL);
	if (branch_exists(name))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" already exists", name)));

	/*
	 * A branch starts at its start point's head commit, as in Dolt: a
	 * branch's uncommitted changes stay its own.  It has only the head
	 * until it is first used.
	 */
	from_exists = branch_exists(from);
	if (from_exists ? !branch_head(from, &id) : !copies_resolve(from, &id))
	{
		ProllyNodeRef baseline;

		if (from_exists)
		{
			/*
			 * (a branch without commits, so with its copies: a copy of it,
			 * as it is)
			 */
			branch_ops()->create(name, from);
			branch_set_baseline(name, branch_baseline(from, &baseline) ?
								&baseline : NULL, NULL);
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch or commit \"%s\" does not exist", from)));
	}
	if (!copies_commit_get(&id, &info))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch or commit \"%s\" does not exist", from)));
	branch_set_head(name, &id);
	branch_set_baseline(name, NULL, NULL);
}

static void
copies_branch_delete(const char *name, bool force)
{
	ProllyHash	head;
	ProllyHash	ours;
	ProllyHash	base;

	lock_branch(name);
	lock_refs();

	/*
	 * Merged, as in Dolt (and git): into the branch's upstream, read from
	 * its remote-tracking branch, so only as current as the last fetch; or,
	 * when it has none there, into the session's branch.  Not asked of the
	 * session's own branch or main, which the drop refuses whatever they
	 * contain: refused first, as Dolt (and git) refuses the checked-out
	 * branch, so the error isn't one -D doesn't get past.
	 */
	if (!force && branch_head(name, &head) &&
		strcmp(name, branch_ops()->current()) != 0 &&
		branch_oid(name) != BRANCH_MAIN_OID &&
		!((upstream_head(name, &ours) ||
		   branch_head(branch_ops()->current(), &ours)) &&
		  copies_merge_base(&head, &ours, &base) && same_hash(&base, &head)))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" is not fully merged", name),
				 errhint("Delete it anyway with -D.")));
	if (OidIsValid(branch_oid(name)) || !branch_head(name, &head))
		branch_ops()->drop(name);	/* (which refuses one that doesn't exist) */
	else
		branch_ops()->check_drop(name); /* (one with only a head) */
	branch_set_head(name, NULL);
	branch_set_baseline(name, NULL, NULL);

	/*
	 * and the merge in progress on it, and its violations, which a branch
	 * made later isn't in
	 */
	{
		StringInfoData key;
		ProllyMergeState ms;

		initStringInfo(&key);
		merge_key(&key, name);
		if (merge_state_of(name, &ms))
		{
			refs_put(key.data, key.len, NULL);
			merge_names_put(name, NULL);
		}
		if (violations_of(name) != NIL)
		{
			resetStringInfo(&key);
			violations_key(&key, name);
			refs_put(key.data, key.len, NULL);
		}
		resetStringInfo(&key);
		names_key(&key, name);
		refs_put(key.data, key.len, NULL);
		pfree(key.data);
	}
}

/*
 * Rename a branch: its catalog row (one with copies), then each of its
 * entries, moved to the new name.  A rename to its own name changes
 * nothing.
 */
static void
copies_branch_rename(const char *name, const char *newname, bool force)
{
	static const char tags[] = {TAG_BRANCH, TAG_BASELINE, TAG_MERGE,
	TAG_MERGE_NAMES, TAG_VIOLATIONS, TAG_UPSTREAM, TAG_MOVER, TAG_BASELINE_MOVER,
	TAG_NAMES};
	ListCell   *lc;

	lock_branch(name);
	lock_branch(newname);
	lock_refs();
	if (!branch_exists(name))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", name)));
	if (strcmp(name, newname) == 0)
		return;
	refuse_commit_name(newname);
	refuse_other_case(newname, name);

	/*
	 * Forced, it takes the place of a branch of the new name, as in Dolt;
	 * not one whose name differs only by case, refused above either way:
	 * the user named another branch.
	 */
	if (branch_exists(newname) && !force)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" already exists", newname),
				 errhint("Replace it with -f.")));
	if (branch_exists(newname))
		copies_branch_delete(newname, true);
	if (OidIsValid(branch_oid(name)))
		branch_ops()->rename(name, newname, force);
	else
	{
		/* (one with only a head: no session is on it) */
		branch_ops()->check_drop(name);
		branch_ops()->check_create(newname);
	}
	for (int i = 0; i < lengthof(tags); i++)
	{
		StringInfoData key;
		StringInfoData v;

		initStringInfo(&key);
		initStringInfo(&v);
		appendStringInfoChar(&key, tags[i]);
		appendStringInfoString(&key, name);

		/*
		 * (what the source hasn't, the new name hasn't either: a branch it
		 * replaced may have left its upstream there)
		 */
		if (refs_get(key.data, key.len, &v))
		{
			refs_put(key.data, key.len, NULL);
			resetStringInfo(&key);
			appendStringInfoChar(&key, tags[i]);
			appendStringInfoString(&key, newname);
			refs_put(key.data, key.len, &v);
		}
		else
		{
			resetStringInfo(&key);
			appendStringInfoChar(&key, tags[i]);
			appendStringInfoString(&key, newname);
			refs_put(key.data, key.len, NULL);
		}
		pfree(key.data);
		pfree(v.data);
	}
	/* (a merge state the transaction set is checked under the new name) */
	foreach(lc, merge_changed)
	{
		if (strcmp(lfirst(lc), name) == 0)
		{
			MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

			lfirst(lc) = pstrdup(newname);
			MemoryContextSwitchTo(old);
		}
	}
}

static bool
copies_branch_lookup(const char *name, ProllyBranchId *branch)
{
	Oid			oid = branch_oid(name);

	*branch = (ProllyBranchId) oid;	/* (invalid for one with only a head) */
	return OidIsValid(oid) || branch_exists(name);
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

	lock_branch(name);
	pushed = see_head(name);
	fold_from_baseline(root, &next, false);
	if (pushed)
		PopActiveSnapshot();
	branch_set_baseline(name, &next, root);
}

/*
 * copies_checkpoint(), the fold made before the branch's refs are taken:
 * they are held (to the transaction's end) only to see that neither the
 * head nor the baseline moved since the fold began, and to set the
 * baseline, and taken only if free.  If either moved, or another holds
 * the branch's refs (or every ref),
 * nothing is set, and the caller rolls back what the fold consumed; held
 * by another as it begins, nothing is folded at all.  The fold is by a
 * snapshot taken after the baseline and head are read: one taken before
 * would see none of a fold that moved the baseline in between (the check
 * then passing), and read every table in full.
 */
static bool
copies_checkpoint_unless_moved(ProllyNodeRef *root)
{
	char	   *name = branch_ops()->current();
	ProllyNodeRef baseline;
	ProllyNodeRef now;
	ProllyNodeRef next;
	ProllyHash	head;
	ProllyHash	head_now;
	bool		had;
	bool		had_head;
	bool		has;
	bool		has_head;

	if (!try_lock_branch(name, false) ||
		(PROLLY_HAS(env.chunk_store, root_free) &&
		 !env.chunk_store->root_free(env.chunks)))
		return false;
	had = branch_baseline(name, &baseline);
	had_head = branch_head(name, &head);
	PushActiveSnapshot(GetLatestSnapshot());
	fold_from_baseline(root, &next, false);
	PopActiveSnapshot();

	/*
	 * (never waiting for the refs: the fold deleted the log's entries it
	 * consumed, which whoever holds the refs may be waiting to delete too;
	 * held by another, it rolls back and folds again later)
	 */
	if (!try_hold_refs(name))
		return false;
	has = branch_baseline(name, &now);
	has_head = branch_head(name, &head_now);
	if (has != had || (had && !same_hash(&baseline.hash, &now.hash)) ||
		has_head != had_head || (had_head && !same_hash(&head, &head_now)))
		return false;
	branch_set_baseline(name, &next, root);
	return true;
}

/*
 * Leaving a branch leaves its changes in its copies, unfolded: its next
 * fold (a commit, a status, another session's read) reads them from its
 * change finder's record, as it would have had the session stayed.  Only a
 * change finder that keeps what it reads changes from until a fold (old
 * versions of pages, fold_on_leave()) takes a working-set checkpoint of a
 * branch with changes as it is left, so what it keeps is freed then: if
 * its working set isn't its head's, nor what its baseline's fold made
 * already.  Not in a transaction that can't write, nor in a transaction
 * block (the refs, held to the transaction's end, would hold up others'
 * commits), nor if the branch's head moved unseen by the snapshot (its
 * fold wouldn't be the branch's), nor its baseline (another's checkpoint
 * or fold, which the snapshot can't fold from: it would read every table
 * in full, and replace that one's baseline with an unsettled one).  (A
 * session choosing its first branch, as it connects, leaves none.)  A
 * working set known (#482) is compared without a fold; one not known is
 * folded once, as the checkpoint, kept even if clean (its fold took the
 * finder's record of its changes).
 */
static void
copies_checkout(const char *name)
{
	char	   *current = branch_ops()->current();
	Oid			oid = strcmp(current, name) != 0 ? branch_oid(name) : InvalidOid;
	ProllyHash	only_head;
	const ProllyChangeFinderRoutine *finder = prolly_change_finder();

	/* (one with only a head gets its copies, a connection's too) */
	if (strcmp(current, name) != 0 && !OidIsValid(oid) &&
		branch_head(name, &only_head))
	{
		bool		pushed = !ActiveSnapshotSet();

		if (pushed)
			PushActiveSnapshot(GetTransactionSnapshot());
		materialize(name);
		if (pushed)
			PopActiveSnapshot();
		oid = branch_oid(name);
	}
	/*
	 * (a branch another transaction first used after this one's snapshot,
	 * which can't move: its copies are rows the snapshot doesn't see, so
	 * the branch would read as the one they were copied from)
	 */
	if (strcmp(current, name) != 0 && OidIsValid(oid) &&
		IsolationUsesXactSnapshot() && branch_made_unseen(name))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access: branch \"%s\" was first used since this transaction's snapshot",
						name),
				 errhint("The transaction might succeed if retried.")));
	if (strcmp(current, name) != 0 && ActiveSnapshotSet() &&
		!XactReadOnly && !RecoveryInProgress() && !IsTransactionBlock() &&
		OidIsValid(oid) && PROLLY_HAS(finder, fold_on_leave) &&
		finder->fold_on_leave())
	{
		ProllyNodeRef working;
		ProllyNodeRef kept;
		ProllyNodeRef next;
		ProllyNodeRef baseline;
		ProllyHash	head;

		if (!(known_working_set(current, MyBranch,
								branch_baseline(current, &baseline) ? &baseline : NULL,
								false, &working) &&
			  ((branch_head(current, &head) &&
				copies_commit_working_set(&head, &kept) &&
				same_hash(&kept.hash, &working.hash)) ||
			   (branch_baseline_root(current, &kept) &&
				same_hash(&kept.hash, &working.hash)))))
		{
			lock_refs();
			if (!head_moved_unseen(current) &&
				!moved_unseen(TAG_BASELINE_MOVER, current))
			{
				fold_from_baseline(&working, &next, false);
				branch_set_baseline(current, &next, &working);
			}
		}
	}
	branch_ops()->checkout(name);
}

/*
 * Commit the session's branch; ending a merge in progress, once its
 * conflicts are resolved, with both heads as parents.  Emptiness is decided
 * before anything is recorded, so a refused commit leaves the head, the
 * baseline and a merge in progress as they were.
 */
static bool
copies_commit(const char *message, bool allow_empty, ProllyHash *commit_id)
{
	ProllyHash	parents[2];
	int			nparents = 0;
	ProllyMergeState ms;
	bool		merging;

	lock_branch(branch_ops()->current());
	if (branch_head(branch_ops()->current(), &parents[0]))
		nparents++;
	if (!prolly_allow_commit_conflicts() && copies_violations() != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot commit with constraint violations"),
				 errhint("Fix the rows and check them again with dolt_verify_constraints().")));
	merging = copies_merge_state(&ms);
	if (merging)
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
	}
	if (!commit_branch(message, parents, nparents, allow_empty, commit_id))
		return false;
	if (merging)
		copies_set_merge_state(NULL);
	return true;
}

/*
 * The head with the transaction's own row changes: no snapshot is checked
 * (heap's row locks, unique indexes and isolation level have decided which
 * changes commit together).  The baseline advances with it for each table
 * whose baseline held the old head's rows: then the branch's next fold, as
 * a status's, a checkout's or the next working-set commit's, reads only the
 * changes since this commit, not every one since the last fold.  A table
 * whose baseline lags the head keeps it, till the next fold advances it.
 */
static int
copies_commit_rows(const char *message, const ProllyRowChanges *changes,
				   int nchanges, const Oid *sequences, int nsequences,
				   ProllyHash *commit_id)
{
	char	   *name = branch_ops()->current();
	ProllyHash	head;
	ProllyCommitInfo info;
	ProllyMergeState ms;
	ProllyNodeRef root;
	ProllyDatabaseComponents dc;
	char	   *names;

	lock_branch(name);
	if (!branch_head(name, &head) || !copies_commit_get(&head, &info) ||
		copies_merge_state(&ms))
		return -1;
	if (!prolly_allow_commit_conflicts() && copies_violations() != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot commit with constraint violations"),
				 errhint("Fix the rows and check them again with dolt_verify_constraints().")));
	/* (each table named as the head has it: else, renamed since, the working set) */
	names = head_names(name, &head);
	if (names == NULL)
		return -1;
	for (int i = 0; i < nchanges; i++)
	{
		Relation	rel = try_table_open(changes[i].relid, AccessShareLock);
		bool		same = rel != NULL && named_as_at_head(names, rel);

		if (rel)
			table_close(rel, AccessShareLock);
		if (!same)
			return -1;
	}
	/* An extension update can change the order without changing the schema. */
	if (!prolly_ordering_dependencies_match(&info))
		return -1;
	prolly_database_components(&dc);
	if (!PROLLY_CALL(dc.canonicalizer, fold_rows, &info.root, changes, nchanges,
					 sequences, nsequences, &root))
		return -1;
	if (same_hash(&root.hash, &info.root.hash))
		return 0;
	write_commit(&root.hash, &head, 1, prolly_commit_author(), message,
				 commit_id);
	branch_set_head(name, commit_id);
	carry_head_names(name, &head, commit_id);
	{
		ProllyNodeRef baseline;
		ProllyNodeRef next;

		if (PROLLY_HAS(dc.canonicalizer, advance_baseline) &&
			branch_baseline(name, &baseline) &&
			PROLLY_CALL(dc.canonicalizer, advance_baseline, &baseline,
						&info.root, &root, changes, nchanges, &next))
			branch_set_baseline(name, &next, NULL);
	}
	return 1;
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
		(void) commit_branch(message, parents, nparents, true, commit_id);
		return;
	}
	name = branch_ops()->current();
	pushed = hold_for_commit(name, parents, nparents);
	fold_from_baseline(&root, &next, true);
	if (pushed)
		PopActiveSnapshot();
	write_commit(&working_set->hash, parents, nparents, prolly_commit_author(),
				 message, commit_id);
	branch_set_head(name, commit_id);
	branch_set_baseline(name, &next, &root);
	note_head_names(name, commit_id);
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
visit_head_only_branch(const char *name, void *arg)
{
	visit_branch(name, InvalidOid, arg);
}

static void
copies_branches(ProllyBranchVisit visit, void *arg)
{
	VisitBranches vb = {visit, arg};

	refs_init();
	branch_ops()->branches(visit_branch, &vb);
	visit_head_only(visit_head_only_branch, &vb);
}

/* A branch's versioned schemas as they are, folded: its working set. */
static void
fold_into(void *arg)
{
	working_fold((ProllyNodeRef *) arg);
}

/*
 * The session branch's working set read: see read_working_set().  Taken
 * from an earlier fold if known, unless fresh (#482); published if of
 * every table and nothing was left out.
 */
typedef struct ReadFold
{
	Oid			only;
	ProllyNodeRef *root;
	List	  **left_out;
	bool		fresh;
} ReadFold;

static void
read_fold_into(void *arg)
{
	ReadFold   *rf = arg;
	ProllyDatabaseComponents dc;
	ProllyNodeRef baseline;
	char	   *name = branch_ops()->current();
	bool		have = branch_baseline(name, &baseline);
	Snapshot	snap;
	BranchFoldStamp stamp;
	List	   *left_out = NIL;

	if (!rf->fresh &&
		known_working_set(name, MyBranch, have ? &baseline : NULL, true,
						  rf->root))
		return;
	snap = fold_snapshot();
	branch_fold_stamp(MyBranch, &stamp);
	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, read_fold, have ? &baseline : NULL, rf->only,
				rf->root, &left_out);
	if (!OidIsValid(rf->only) && left_out == NIL)
		publish_fold(&stamp, snap, have ? &baseline : NULL, rf->root, true);
	if (rf->left_out)
		*rf->left_out = list_concat(*rf->left_out, left_out);
}

/* (the fresh fold of the session's branch verify_known() compares with) */
static void
fresh_fold_into(void *arg)
{
	ReadFold   *rf = arg;

	if (rf->left_out)
		read_fold_into(rf);
	else
		fold(rf->root);
}

/*
 * debug_prolly_verify_fold_caches: the working set taken for branch name,
 * folded again and compared (prolly_check_fold_cache()); complete, a read
 * fold's, which must leave nothing out.
 */
static void
verify_known(const char *name, bool complete, const ProllyNodeRef *root)
{
	ProllyNodeRef folded;
	List	   *left_out = NIL;
	ReadFold	rf = {InvalidOid, &folded, complete ? &left_out : NULL, true};

	if (strcmp(name, branch_ops()->current()) == 0)
		fresh_fold_into(&rf);
	else
		branch_ops()->run_on(name, fresh_fold_into, &rf);
	prolly_check_fold_cache(name, root, &folded,
							left_out != NIL ? (const char *) linitial(left_out) : NULL);
}

/* A branch's working set as a head-only one has it: false if none. */
static bool
head_working_set(const char *branch, ProllyNodeRef *root)
{
	ProllyHash	head;
	ProllyCommitInfo info;

	if (!branch_head(branch, &head) || !copies_commit_get(&head, &info))
		return false;
	*root = info.root;
	return true;
}

/*
 * (another branch's working set known is taken without checking it out: a
 * clean one's dirtiness is known without folding (#482))
 */
static bool
copies_read_working_set(const char *branch, Oid only, ProllyNodeRef *root,
						List **left_out)
{
	ReadFold	rf = {only, root, left_out, false};
	Oid			oid;
	ProllyNodeRef baseline;

	if (branch == NULL)
	{
		read_fold_into(&rf);
		return true;
	}
	oid = branch_oid(branch);
	if (!OidIsValid(oid))
		return head_working_set(branch, root);
	if (known_working_set(branch, oid,
						  branch_baseline(branch, &baseline) ? &baseline : NULL,
						  true, root))
		return true;
	branch_ops()->run_on(branch, read_fold_into, &rf);
	return true;
}

/* What a read of definitions began at, for publishing its root. */
typedef struct DefinitionsRead
{
	BranchFoldStamp stamp;
	Snapshot	snap;
	bool		have;
	ProllyNodeRef baseline;
} DefinitionsRead;

/*
 * The session branch's definitions: its working set known if any (the rows
 * in it are right too), else a fold of no table's rows.  That isn't the
 * working set, unless no table's rows changed since: *read says what the
 * read began at, for publish_definitions() to publish it then.
 */
static void
copies_read_definitions(ProllyNodeRef *root, List **left_out, bool *rows,
						void **read)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef baseline;
	char	   *name = branch_ops()->current();
	bool		have = branch_baseline(name, &baseline);

	if (rows)
		*rows = true;
	if (read)
		*read = NULL;
	if (known_working_set(name, MyBranch, have ? &baseline : NULL, true,
						  root))
		return;
	prolly_database_components(&dc);
	if (!PROLLY_HAS(dc.canonicalizer, read_definitions))
	{
		ReadFold	rf = {InvalidOid, root, left_out, false};

		read_fold_into(&rf);
		return;
	}
	if (rows)
		*rows = false;
	if (read)
	{
		DefinitionsRead *dr = palloc0(sizeof(DefinitionsRead));

		branch_fold_stamp(MyBranch, &dr->stamp);
		dr->snap = fold_snapshot();
		dr->have = have;
		if (have)
			dr->baseline = baseline;
		*read = dr;
	}
	dc.canonicalizer->read_definitions(have ? &baseline : NULL, root,
									   left_out);
}

/*
 * The session branch's changes to table relid since its baseline, read
 * without a fold; none if its working set is known, which a read takes
 * as it is.
 */
static bool
copies_working_changes(Oid relid, ProllyNodeRef *data,
					   ProllyRowChangeVisit visit, void *arg)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef baseline;
	ProllyNodeRef known;
	char	   *name = branch_ops()->current();

	/* (asked without taking it: the read that follows takes it) */
	if (!branch_baseline(name, &baseline) ||
		(fold_caches_usable() &&
		 branch_fold_known(MyBranch, fold_snapshot(), &baseline.hash, true,
						   &known.hash)))
		return false;
	prolly_database_components(&dc);
	if (!PROLLY_HAS(dc.canonicalizer, table_changes))
		return false;
	return dc.canonicalizer->table_changes(&baseline, relid, data, visit, arg);
}

/*
 * The transaction's own row changes folded into the session branch's
 * baseline, under the branch's refs (so its baseline moves one transaction
 * at a time).  Only if they're free: the fold is an optimization a commit mustn't wait
 * for (nor deadlock or time out on); held by another, the changes are left
 * to the next fold, as without it.
 */
static bool
copies_fold_own(const ProllyRowChanges *changes, int nchanges)
{
	char	   *name = branch_ops()->current();
	ProllyNodeRef baseline;
	ProllyNodeRef next;
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	if (!PROLLY_HAS(dc.canonicalizer, advance_own) || nchanges == 0)
		return false;
	if (!try_hold_refs(name))
		return false;
	if (!branch_baseline(name, &baseline) ||
		!dc.canonicalizer->advance_own(&baseline, changes, nchanges, &next))
		return false;
	branch_set_baseline(name, &next, NULL);
	return true;
}

static void
copies_publish_definitions(void *read, const ProllyNodeRef *root,
						   bool complete)
{
	DefinitionsRead *dr = read;

	if (dr != NULL)
		publish_fold(&dr->stamp, dr->snap, dr->have ? &dr->baseline : NULL,
					 root, complete);
}

static bool
copies_working_set(const char *branch, ProllyNodeRef *root)
{
	if (branch == NULL)
	{
		working_fold(root);
		return true;
	}
	/* (one with only a head: its working set is the head's) */
	if (!OidIsValid(branch_oid(branch)))
		return head_working_set(branch, root);
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
	move_settled(&info.root);
	if (pushed)
		PopActiveSnapshot();
	branch_set_head(branch_ops()->current(), &target);
	copies_set_merge_state(NULL);	/* a reset ends a merge */
	copies_set_violations(NIL);
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

/*
 * A branch's merge's conflicted tables' names, as the merge recorded them,
 * under a key of their own (the merge state's value ends with a message):
 * each table's OID, then its name's length (a varint) and bytes.
 */
static void
merge_names_key(StringInfo key, const char *branch)
{
	appendStringInfoChar(key, TAG_MERGE_NAMES);
	appendStringInfoString(key, branch);
}

static void
merge_names_put(const char *branch, const ProllyMergeState *state)
{
	StringInfoData key;
	StringInfoData v;
	bool		any = false;

	initStringInfo(&key);
	initStringInfo(&v);
	merge_names_key(&key, branch);
	for (int i = 0; state && state->table_names && i < state->ntables; i++)
		if (state->table_names[i])
		{
			put_be32(&v, state->tables[i]);
			prolly_put_varint(&v, strlen(state->table_names[i]));
			appendStringInfoString(&v, state->table_names[i]);
			any = true;
		}
	{
		StringInfoData had;

		initStringInfo(&had);
		if (any || refs_get(key.data, key.len, &had))
			refs_put(key.data, key.len, any ? &v : NULL);
		pfree(had.data);
	}
	pfree(key.data);
	pfree(v.data);
}

/* state's table_names, from the key above (none recorded: NULL) */
static void
merge_names_get(const char *branch, ProllyMergeState *state)
{
	StringInfoData key;
	StringInfoData v;

	initStringInfo(&key);
	initStringInfo(&v);
	merge_names_key(&key, branch);
	state->table_names = NULL;
	if (refs_get(key.data, key.len, &v))
	{
		const char *p = v.data;
		const char *end = v.data + v.len;

		state->table_names = palloc0(sizeof(char *) * Max(state->ntables, 1));
		while (end - p >= 4)
		{
			Oid			oid = get_be32(p);
			uint64		len;

			p += 4;
			len = prolly_get_varint(&p, end);
			if (len > (uint64) (end - p))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("prolly refs hold an invalid merge state")));
			for (int i = 0; i < state->ntables; i++)
				if (state->tables[i] == oid)
					state->table_names[i] = pnstrdup(p, len);
			p += len;
		}
	}
	pfree(key.data);
	pfree(v.data);
}

/*
 * The merge in progress on branch, if there is one, as the refs hold it:
 * its conflicted tables, as merge_tables_left() has them, may include some
 * since dropped.
 */
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
		if (end - p < 4)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly refs hold an invalid merge state")));
		state->ntables = get_be32(p);
		p += 4;
		if (end - p < (int64) state->ntables * 4)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly refs hold an invalid merge state")));
		state->tables = palloc(sizeof(Oid) * Max(state->ntables, 1));
		for (int i = 0; i < state->ntables; i++, p += 4)
			state->tables[i] = get_be32(p);
		/* a cherry-pick's message; a revert's series, then its message */
		state->pick = p < end;
		if (state->pick && *p == 'r')
		{
			state->revert = true;
			p = get_hash(p + 1, end, &state->series_head);
			if (end - p < 4)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("prolly refs hold an invalid merge state")));
			state->npending = get_be32(p);
			p += 4;
			state->pending = palloc(sizeof(ProllyHash) * Max(state->npending, 1));
			for (int i = 0; i < state->npending; i++)
				p = get_hash(p, end, &state->pending[i]);
			state->message = pnstrdup(p, end - p);
		}
		else
			state->message = state->pick ? pnstrdup(p + 1, end - p - 1) : NULL;
	}
	if (found)
		merge_names_get(branch, state);
	pfree(key.data);
	pfree(v.data);
	return found;
}

/*
 * Leave out of a branch's merge state the conflicted tables the branch has
 * since dropped (as Dolt, dropping a table drops its conflicts); name: the
 * first one left's name, if wanted.
 */
static void
merge_tables_left(const char *branch, ProllyMergeState *state, char **name)
{
	bool	   *exists = palloc(sizeof(bool) * Max(state->ntables, 1));
	int			n = 0;

	tables_exist_on(branch, state->ntables, state->tables, exists, name);
	for (int i = 0; i < state->ntables; i++)
		if (exists[i])
		{
			if (state->table_names)
				state->table_names[n] = state->table_names[i];
			state->tables[n++] = state->tables[i];
		}
	state->ntables = n;
	pfree(exists);
}

static bool
copies_merge_state(ProllyMergeState *state)
{
	char	   *branch = branch_ops()->current();

	if (!merge_state_of(branch, state))
		return false;
	merge_tables_left(branch, state, NULL);
	return true;
}

/* (NULL: none) */
static void
copies_set_merge_state(const ProllyMergeState *state)
{
	StringInfoData key;
	StringInfoData v;
	ProllyMergeState cur;
	char	   *branch = branch_ops()->current();
	bool		had;

	refs_init();
	initStringInfo(&key);
	merge_key(&key, branch);
	had = merge_state_of(branch, &cur);
	if (state == NULL)
	{
		if (had)
			refs_put(key.data, key.len, NULL);
		merge_names_put(branch, NULL);
	}
	else
	{
		initStringInfo(&v);
		put_hash(&v, &state->theirs);
		put_hash(&v, &state->base);
		put_be32(&v, state->ntables);
		for (int i = 0; i < state->ntables; i++)
			put_be32(&v, state->tables[i]);
		if (state->revert)
		{
			appendStringInfoChar(&v, 'r');
			put_hash(&v, &state->series_head);
			put_be32(&v, state->npending);
			for (int i = 0; i < state->npending; i++)
				put_hash(&v, &state->pending[i]);
			appendStringInfoString(&v, state->message ? state->message : "");
		}
		else if (state->pick)
		{
			appendStringInfoChar(&v, 'p');
			appendStringInfoString(&v, state->message ? state->message : "");
		}
		refs_put(key.data, key.len, &v);
		pfree(v.data);
		merge_names_put(branch, state);
		note_merge_changed(branch);
	}
	pfree(key.data);
}

static void
violations_key(StringInfo key, const char *branch)
{
	appendStringInfoChar(key, TAG_VIOLATIONS);
	appendStringInfoString(key, branch);
}

static void
put_text(StringInfo out, const char *s)
{
	put_be32(out, (uint32) strlen(s));
	appendStringInfoString(out, s);
}

static char *
get_text(const char **p, const char *end)
{
	uint32		len;
	char	   *s;

	if (end - *p < 4)
		goto corrupt;
	len = get_be32(*p);
	*p += 4;
	if ((uint32) (end - *p) < len)
		goto corrupt;
	s = pnstrdup(*p, len);
	*p += len;
	return s;

corrupt:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("a branch's constraint violations are corrupt")));
	return NULL;				/* keep compiler quiet */
}

/*
 * The constraint violations a merge left on branch, as the refs hold them:
 * violations_left() leaves out those of tables since dropped.
 */
static List *
violations_of(const char *branch)
{
	StringInfoData key;
	StringInfoData v;
	List	   *list = NIL;

	refs_init();
	initStringInfo(&key);
	initStringInfo(&v);
	violations_key(&key, branch);
	if (refs_get(key.data, key.len, &v))
	{
		const char *p = v.data;
		const char *end = v.data + v.len;

		while (p < end)
		{
			ProllyViolation *pv = palloc0(sizeof(ProllyViolation));

			if (end - p < 4)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("a branch's constraint violations are corrupt")));
			pv->relid = get_be32(p);
			p += 4;
			pv->type = get_text(&p, end);
			pv->row = get_text(&p, end);
			pv->info = get_text(&p, end);
			list = lappend(list, pv);
		}
	}
	pfree(key.data);
	pfree(v.data);
	return list;
}

/*
 * Of a branch's constraint violations, those of tables it still has (a
 * dropped table has no rows to fix); name: the first one's table's name,
 * if wanted.
 */
static List *
violations_left(const char *branch, List *violations, char **name)
{
	int			n = list_length(violations);
	Oid		   *relids = palloc(sizeof(Oid) * Max(n, 1));
	bool	   *exists = palloc(sizeof(bool) * Max(n, 1));
	List	   *left = NIL;
	ListCell   *lc;

	foreach(lc, violations)
		relids[foreach_current_index(lc)] = ((ProllyViolation *) lfirst(lc))->relid;
	tables_exist_on(branch, n, relids, exists, name);
	foreach(lc, violations)
		if (exists[foreach_current_index(lc)])
			left = lappend(left, lfirst(lc));
	pfree(relids);
	pfree(exists);
	return left;
}

static List *
copies_violations(void)
{
	char	   *branch = branch_ops()->current();

	return violations_left(branch, violations_of(branch), NULL);
}

static void
copies_set_violations(List *violations)
{
	StringInfoData key;
	StringInfoData v;
	ListCell   *lc;

	refs_init();
	initStringInfo(&key);
	violations_key(&key, branch_ops()->current());
	if (violations == NIL)
	{
		StringInfoData had;

		initStringInfo(&had);
		if (refs_get(key.data, key.len, &had))
			refs_put(key.data, key.len, NULL);
		pfree(had.data);
	}
	else
	{
		initStringInfo(&v);
		foreach(lc, violations)
		{
			ProllyViolation *pv = lfirst(lc);

			put_be32(&v, pv->relid);
			put_text(&v, pv->type);
			put_text(&v, pv->row);
			put_text(&v, pv->info);
		}
		refs_put(key.data, key.len, &v);
		pfree(v.data);
	}
	note_merge_changed(branch_ops()->current());
	pfree(key.data);
}

/* ------------------------------------------------------------------------
 * Remotes
 * ------------------------------------------------------------------------
 */

/* Visit every entry whose key starts with prefix, in key order. */
static void
visit_prefix(const char *prefix, int prefixlen,
			 void (*visit) (const ProllyKey *k, const ProllyValue *v, void *arg),
			 void *arg)
{
	ProllyNodeRef root;
	ProllyTreeCursor *c;
	ProllyKey	from = {(const uint8 *) prefix, prefixlen};
	ProllyKey	k;
	ProllyValue v;

	if (!refs_root(&root))
		return;
	c = PROLLY_CALL(tree, cursor_open, &env, &root, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, &from, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v) &&
		   k.len >= prefixlen && memcmp(k.data, prefix, prefixlen) == 0)
		visit(&k, &v, arg);
	PROLLY_CALL(tree, cursor_close, c);
}

/* a ref a root named, for copies_root_refs() to sort */
typedef struct RootRef
{
	char	   *name;
	ProllyHash	commit;
} RootRef;

static int
root_ref_cmp(const void *a, const void *b)
{
	return strcmp(((const RootRef *) a)->name, ((const RootRef *) b)->name);
}

/*
 * The refs a root of the store named, whenever it was the store's root:
 * branches' heads and remote-tracking branches' commits, in the order of
 * their full names (as Dolt's: a tracking branch's key, remote then
 * branch, sorts otherwise where one remote's name begins another's).
 */
static bool
copies_root_refs(const ProllyHash *root, ProllyRootRefVisit visit, void *arg)
{
	static const char tags[] = {TAG_BRANCH, TAG_TRACKING};
	ProllyNodeRef r;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	RootRef    *refs;
	int			nrefs = 0;
	int			maxrefs = 16;

	refs_init();
	if (!PROLLY_CALL(env.chunk_store, has, env.chunks, root))
		return false;
	ref_of(root, &r);
	refs = palloc(sizeof(RootRef) * maxrefs);
	c = PROLLY_CALL(tree, cursor_open, &env, &r, CurrentMemoryContext);
	for (int t = 0; t < lengthof(tags); t++)
	{
		ProllyKey	from = {(const uint8 *) &tags[t], 1};

		PROLLY_CALL(tree, cursor_seek, c, &from, ForwardScanDirection);
		while (PROLLY_CALL(tree, cursor_next, c, &k, &v) &&
			   k.len >= 1 && k.data[0] == tags[t])
		{
			const char *name = (const char *) k.data + 1;
			int			namelen = k.len - 1;
			StringInfoData ref;

			initStringInfo(&ref);
			if (tags[t] == TAG_BRANCH)
			{
				appendStringInfoString(&ref, "refs/heads/");
				appendBinaryStringInfo(&ref, name, namelen);
			}
			else
			{
				int			rlen = strnlen(name, namelen);

				appendStringInfoString(&ref, "refs/remotes/");
				appendBinaryStringInfo(&ref, name, rlen);
				appendStringInfoChar(&ref, '/');
				if (rlen < namelen)
					appendBinaryStringInfo(&ref, name + rlen + 1, namelen - rlen - 1);
			}
			if (nrefs == maxrefs)
			{
				maxrefs *= 2;
				refs = repalloc(refs, sizeof(RootRef) * maxrefs);
			}
			refs[nrefs].name = ref.data;
			(void) get_hash((const char *) v.data, (const char *) v.data + v.len,
							&refs[nrefs].commit);
			nrefs++;
		}
	}
	PROLLY_CALL(tree, cursor_close, c);
	qsort(refs, nrefs, sizeof(RootRef), root_ref_cmp);
	for (int i = 0; i < nrefs; i++)
	{
		visit(refs[i].name, &refs[i].commit, arg);
		pfree(refs[i].name);
	}
	pfree(refs);
	return true;
}

static void
tracking_key(StringInfo key, const char *remote, const char *branch)
{
	appendStringInfoChar(key, TAG_TRACKING);
	appendStringInfoString(key, remote);
	appendStringInfoChar(key, '\0');
	if (branch)
		appendStringInfoString(key, branch);
}

/*
 * refname as a remote-tracking branch, "remote/branch" or
 * "remotes/remote/branch": its commit; false if it is none.
 */
static bool
tracking_head(const char *refname, ProllyHash *commit_id)
{
	const char *name = refname;
	const char *slash;
	StringInfoData key;
	StringInfoData v;
	bool		found;

	if (strncmp(name, "remotes/", 8) == 0)
		name += 8;
	slash = strchr(name, '/');
	if (slash == NULL || slash == name || slash[1] == '\0')
		return false;
	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_TRACKING);
	appendBinaryStringInfo(&key, name, slash - name);
	appendStringInfoChar(&key, '\0');
	appendStringInfoString(&key, slash + 1);
	found = refs_get(key.data, key.len, &v);
	if (found)
		(void) get_hash(v.data, v.data + v.len, commit_id);
	pfree(key.data);
	pfree(v.data);
	return found && commit_id->len > 0;
}

typedef struct RemoteVisit
{
	ProllyRemoteVisit visit;
	void	   *arg;
} RemoteVisit;

static void
visit_remote_entry(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	RemoteVisit *rv = arg;
	char	   *name = pnstrdup((const char *) k->data + 1, k->len - 1);
	char	   *url = pnstrdup((const char *) v->data, v->len);

	rv->visit(name, url, rv->arg);
	pfree(name);
	pfree(url);
}

static void
copies_remotes(ProllyRemoteVisit visit, void *arg)
{
	RemoteVisit rv = {visit, arg};
	char		tag = TAG_REMOTE;

	refs_init();
	visit_prefix(&tag, 1, visit_remote_entry, &rv);
}

typedef struct TrackingVisit
{
	ProllyTrackingVisit visit;
	void	   *arg;
} TrackingVisit;

static void
visit_tracking_entry(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	TrackingVisit *tv = arg;
	const char *p = (const char *) k->data + 1;
	int			rlen = strnlen(p, k->len - 1);
	char	   *remote = pnstrdup(p, rlen);
	char	   *branch = pnstrdup(p + rlen + 1, k->len - 2 - rlen);
	ProllyHash	h;

	(void) get_hash((const char *) v->data, (const char *) v->data + v->len, &h);
	tv->visit(remote, branch, &h, tv->arg);
	pfree(remote);
	pfree(branch);
}

static void
copies_trackings(const char *remote, ProllyTrackingVisit visit, void *arg)
{
	TrackingVisit tv = {visit, arg};
	StringInfoData prefix;

	refs_init();
	initStringInfo(&prefix);
	if (remote)
		tracking_key(&prefix, remote, NULL);
	else
		appendStringInfoChar(&prefix, TAG_TRACKING);
	visit_prefix(prefix.data, prefix.len, visit_tracking_entry, &tv);
	pfree(prefix.data);
}

static void
copies_tracking_set(const char *remote, const char *branch,
					const ProllyHash *commit_id)
{
	StringInfoData key;
	StringInfoData v;

	initStringInfo(&key);
	initStringInfo(&v);
	tracking_key(&key, remote, branch);
	if (commit_id)
		put_hash(&v, commit_id);
	refs_put(key.data, key.len, commit_id ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
}

static void
collect_key(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	List	  **keys = arg;
	StringInfo	key = makeStringInfo();

	appendBinaryStringInfo(key, k->data, k->len);
	*keys = lappend(*keys, key);
}

typedef struct UpstreamsOf
{
	const char *remote;
	List	   *keys;
} UpstreamsOf;

static void
collect_upstream_of(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	UpstreamsOf *u = arg;

	if (strnlen((const char *) v->data, v->len) == strlen(u->remote) &&
		memcmp(v->data, u->remote, strlen(u->remote)) == 0)
		collect_key(k, v, &u->keys);
}

static void
copies_remote_set(const char *name, const char *url)
{
	StringInfoData key;
	StringInfoData v;

	refs_init();
	initStringInfo(&key);
	appendStringInfoChar(&key, TAG_REMOTE);
	appendStringInfoString(&key, name);
	if (url)
	{
		initStringInfo(&v);
		appendStringInfoString(&v, url);
		refs_put(key.data, key.len, &v);
		pfree(v.data);
	}
	else
	{
		List	   *keys = NIL;
		UpstreamsOf u = {name, NIL};
		StringInfoData prefix;
		char		tag = TAG_UPSTREAM;
		ListCell   *lc;

		/* its remote-tracking branches and the upstreams naming it go too */
		initStringInfo(&prefix);
		tracking_key(&prefix, name, NULL);
		visit_prefix(prefix.data, prefix.len, collect_key, &keys);
		visit_prefix(&tag, 1, collect_upstream_of, &u);
		keys = list_concat(keys, u.keys);
		foreach(lc, keys)
		{
			StringInfo	k = lfirst(lc);

			refs_put(k->data, k->len, NULL);
		}
		refs_put(key.data, key.len, NULL);
		pfree(prefix.data);
	}
	pfree(key.data);
}

static void
copies_upstream_set(const char *branch, const char *remote,
					const char *remote_branch)
{
	StringInfoData key;
	StringInfoData v;

	refs_init();
	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_UPSTREAM);
	appendStringInfoString(&key, branch);
	if (remote)
	{
		appendStringInfoString(&v, remote);
		appendStringInfoChar(&v, '\0');
		appendStringInfoString(&v, remote_branch);
	}
	refs_put(key.data, key.len, remote ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
}

static bool
copies_upstream(const char *branch, char **remote, char **remote_branch)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;

	refs_init();
	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_UPSTREAM);
	appendStringInfoString(&key, branch);
	found = refs_get(key.data, key.len, &v);
	if (found)
	{
		int			rlen = strnlen(v.data, v.len);

		*remote = pnstrdup(v.data, rlen);
		*remote_branch = pnstrdup(v.data + rlen + 1, v.len - rlen - 1);
	}
	pfree(key.data);
	pfree(v.data);
	return found;
}

/*
 * The commit of branch's upstream's remote-tracking branch; false if it has
 * no upstream, or the upstream no remote-tracking branch (not yet fetched).
 */
static bool
upstream_head(const char *branch, ProllyHash *commit_id)
{
	char	   *remote;
	char	   *remote_branch;
	StringInfoData key;
	StringInfoData v;
	bool		found;

	if (!copies_upstream(branch, &remote, &remote_branch))
		return false;
	initStringInfo(&key);
	initStringInfo(&v);
	tracking_key(&key, remote, remote_branch);
	found = refs_get(key.data, key.len, &v);
	if (found)
		(void) get_hash(v.data, v.data + v.len, commit_id);
	pfree(key.data);
	pfree(v.data);
	pfree(remote);
	pfree(remote_branch);
	return found && commit_id->len > 0;
}

static bool
copies_config_get(const char *name, StringInfo value)
{
	StringInfoData key;
	bool		found;

	refs_init();
	initStringInfo(&key);
	appendStringInfoChar(&key, TAG_CONFIG);
	appendStringInfoString(&key, name);
	found = refs_get(key.data, key.len, value);
	pfree(key.data);
	return found;
}

static void
copies_config_set(const char *name, const char *value)
{
	StringInfoData key;
	StringInfoData v;

	refs_init();
	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_CONFIG);
	appendStringInfoString(&key, name);
	if (value)
		appendStringInfoString(&v, value);
	refs_put(key.data, key.len, value ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
}

typedef struct WalkState
{
	ProllyTreeVisitNode visit;
	void	   *arg;
	HTAB	   *seen;			/* ProllyHash: commit ids walked */
} WalkState;

/*
 * Walk the commit id and its ancestors, each once: a work list, not
 * recursion (a history may be as long as it likes), and a hash set of the
 * commits seen.  A commit missing from the store is an error; a chunk that
 * isn't a commit is passed over, nothing of it visited.
 */
static void
walk_commit(WalkState *ws, const ProllyHash *id)
{
	ProllyDatabaseComponents dc;
	List	   *todo = list_make1(palloc(sizeof(ProllyHash)));

	*(ProllyHash *) linitial(todo) = *id;
	if (ws->seen == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(ProllyHash);
		ctl.entrysize = sizeof(ProllyHash);
		ctl.hcxt = CurrentMemoryContext;
		ws->seen = hash_create("prolly commits walked", 256, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	prolly_database_components(&dc);
	while (todo != NIL)
	{
		ProllyHash *next = llast(todo);
		ProllyHash	key;
		ProllyCommitInfo info;
		ProllyNodeRef ref;
		bool		found;

		todo = list_delete_last(todo);
		memset(&key, 0, sizeof(key));
		key.len = next->len;
		memcpy(key.bytes, next->bytes, next->len);
		(void) hash_search(ws->seen, &key, HASH_ENTER, &found);
		if (found || next->len == 0)
			continue;

		/*
		 * A commit named that the store hasn't: the store isn't whole (a
		 * transfer's walk must not take it as present, nor reclamation go on
		 * as if it were).  A chunk that isn't a commit: nothing of it is
		 * visited.
		 */
		if (!PROLLY_CALL(env.chunk_store, has, env.chunks, next))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("commit %s is missing from the chunk store",
							prolly_hash_hex(next))));
		if (!copies_commit_get(next, &info))
			continue;

		/* (a commit the visitor declines: it has seen its root and parents) */
		ref_of(next, &ref);
		if (!PROLLY_CALL(tree, walk, &env, &ref, ws->visit, ws->arg))
			continue;
		PROLLY_CALL(dc.canonicalizer, walk, &info.root, ws->visit, ws->arg);
		if (info.has_dependencies)
			(void) PROLLY_CALL(tree, walk, &env, &info.dependencies, ws->visit,
							   ws->arg);
		for (int i = info.nparents - 1; i >= 0; i--)
		{
			ProllyHash *p = palloc(sizeof(ProllyHash));

			*p = info.parents[i];
			todo = lappend(todo, p);
		}
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * The refs tree, every commit reachable from a head, a remote-tracking
 * branch or a merge in progress, with its root, and every baseline.
 */
static void
copies_walk(const ProllyNodeRef *refs, ProllyTreeVisitNode visit, void *arg)
{
	WalkState	ws = {visit, arg, NULL};
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

		/* (a stash list: each one's commit, kept as a head is) */
		if (k.len >= 1 && k.data[0] == TAG_STASH)
		{
			while (p < end)
			{
				uint64		blen;

				h = palloc(sizeof(ProllyHash));
				p = get_hash(p, end, h);
				heads = lappend(heads, h);
				blen = prolly_get_varint(&p, end);
				if (blen > (uint64) (end - p))
					break;
				p += blen;
			}
			continue;
		}
		if (k.len < 1 || (k.data[0] != TAG_BRANCH && k.data[0] != TAG_MERGE &&
						  k.data[0] != TAG_BASELINE && k.data[0] != TAG_TRACKING))
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

static void
copies_walk_commit(const ProllyHash *commit_id, ProllyTreeVisitNode visit,
				   void *arg)
{
	WalkState	ws = {visit, arg, NULL};

	refs_init();
	walk_commit(&ws, commit_id);
}

static bool
copies_hold(void)
{
	lock_refs();
	return see_head(branch_ops()->current());
}

static void
count_table(const char *name, const ProllyNodeRef *data,
			const ProllyValue *description, const char *unreadable, void *arg)
{
	(*(int *) arg)++;
}

static void
count_object(const char *name, ProllyDiffKind kind, char object, void *arg)
{
	(*(int *) arg)++;
}

/*
 * Has a branch with copies changes its head lacks?  One without commits
 * has whatever its working set holds (a versioned schema, though empty).
 */
static bool
branch_dirty(const char *name, const ProllyHash *head)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef ws;
	ProllyNodeRef committed;
	List	   *left_out = NIL;

	if (!copies_read_working_set(name, InvalidOid, &ws, &left_out))
		return false;
	if (left_out != NIL)
		return true;
	if (head->len > 0)
		return !(copies_commit_working_set(head, &committed) &&
				 same_hash(&ws.hash, &committed.hash));
	{
		ProllyTreeBuilder *b = PROLLY_CALL(tree, build_begin, &env,
										   CurrentMemoryContext);
		ProllyNodeRef empty;
		int			n = 0;

		PROLLY_CALL(tree, build_finish, b, &empty);
		prolly_database_components(&dc);
		PROLLY_CALL(dc.canonicalizer, tables, &ws, NULL, count_table, &n);
		PROLLY_CALL(dc.canonicalizer, objects, &empty, &ws, count_object, &n);
		return n > 0;
	}
}

/* End a branch's merge in progress (and its violations), if it has one. */
static void
end_merge(const char *name)
{
	StringInfoData key;
	ProllyMergeState ms;

	initStringInfo(&key);
	if (merge_state_of(name, &ms))
	{
		merge_key(&key, name);
		refs_put(key.data, key.len, NULL);
		merge_names_put(name, NULL);
	}
	if (violations_of(name) != NIL)
	{
		resetStringInfo(&key);
		violations_key(&key, name);
		refs_put(key.data, key.len, NULL);
	}
	pfree(key.data);
}

static bool
copies_branch_move(const char *name, const ProllyHash *expected,
				   const ProllyHash *head, bool force)
{
	ProllyHash	at;
	ProllyCommitInfo info;
	bool		pushed = false;
	Oid			oid;

	lock_branch(name);
	lock_refs();
	AcceptInvalidationMessages();
	(void) branch_head(name, &at);
	if (!same_hash(&at, expected))
		return false;
	if (head && !copies_commit_get(head, &info))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("commit %s is missing", prolly_hash_hex(head))));
	if (!branch_exists(name))
	{
		/* (one already gone: deleted, as asked) */
		if (head == NULL)
			return true;
		/* made at the commit, with only its head */
		branch_ops()->check_create(name);
		branch_set_head(name, head);
		branch_set_baseline(name, NULL, NULL);
		return true;
	}

	/*
	 * A branch with copies: its working set read as of its head (a snapshot
	 * that sees the transaction that moved it there), refused if it has
	 * uncommitted changes unless forced, then moved, its rows written, or
	 * deleted.  Not under a prepared transaction that was on it, refused
	 * as a deletion is: its rows would be overwritten (or waited on, maybe
	 * for days), or come back as changes the move dropped.
	 */
	oid = branch_oid(name);
	if (OidIsValid(oid))
	{
		lock_branch_unprepared(oid, name);
		pushed = see_head(name);
		PG_TRY();
		{
			if (!force && branch_dirty(name, &at))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("branch \"%s\" has uncommitted changes", name),
						 errhint("Commit or reset them there, or push with --force, which drops them.")));
			/* (what the commit needs that the branch's head doesn't) */
			if (head)
			{
				ProllyCommitInfo was;

				prolly_check_commit_dependencies(&info,
												 at.len > 0 && copies_commit_get(&at, &was) ?
												 &was : NULL, "push", false);
			}
			if (head == NULL)
				;
			else if (strcmp(name, branch_ops()->current()) == 0)
				move_settled(&info.root);
			else
				branch_ops()->run_on(name, move_settled, &info.root);
		}
		PG_FINALLY();
		{
			if (pushed)
				PopActiveSnapshot();
		}
		PG_END_TRY();
	}
	if (head == NULL)
	{
		/* (whether merged into this session's branch doesn't matter here) */
		copies_branch_delete(name, true);
		return true;
	}
	branch_set_head(name, head);
	end_merge(name);	/* (a move ends a merge in progress there, as a reset does) */

	/*
	 * (forced, its uncommitted changes are gone: a writer that owned it for
	 * them doesn't any more)
	 */
	if (force && OidIsValid(oid))
	{
		ProllyDatabaseComponents dc;

		prolly_database_components(&dc);
		PROLLY_CALL(dc.writers, settled, oid);
	}
	return true;
}

/* ------------------------------------------------------------------------
 * Stashes: TAG_STASH and a list's name, its entries newest first, each its
 * commit's hash, then its branch's name (a varint length, then the bytes).
 * ------------------------------------------------------------------------
 */

typedef struct StashEntry
{
	ProllyHash	stash;
	char	   *branch;
} StashEntry;

static List *
stash_list(const char *p, const char *end)
{
	List	   *entries = NIL;

	while (p < end)
	{
		StashEntry *e = palloc(sizeof(StashEntry));
		uint64		blen;

		p = get_hash(p, end, &e->stash);
		blen = prolly_get_varint(&p, end);
		if (blen > (uint64) (end - p))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupt prolly stash list")));
		e->branch = pnstrdup(p, blen);
		p += blen;
		entries = lappend(entries, e);
	}
	return entries;
}

static void
stash_key(const char *name, StringInfo key)
{
	appendStringInfoChar(key, TAG_STASH);
	appendStringInfoString(key, name);
}

/* name's list, newest first (NIL: none) */
static List *
stash_get(const char *name)
{
	StringInfoData key;
	StringInfoData v;
	List	   *entries = NIL;

	initStringInfo(&key);
	initStringInfo(&v);
	stash_key(name, &key);
	if (refs_get(key.data, key.len, &v))
		entries = stash_list(v.data, v.data + v.len);
	pfree(key.data);
	pfree(v.data);
	return entries;
}

/* name's list made entries (NIL: none), with the transaction */
static void
stash_set(const char *name, List *entries)
{
	StringInfoData key;
	StringInfoData v;
	ListCell   *lc;

	initStringInfo(&key);
	initStringInfo(&v);
	stash_key(name, &key);
	foreach(lc, entries)
	{
		StashEntry *e = lfirst(lc);

		put_hash(&v, &e->stash);
		prolly_put_varint(&v, strlen(e->branch));
		appendStringInfoString(&v, e->branch);
	}
	refs_put(key.data, key.len, entries ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
}

static void
copies_stash_push(const char *name, const ProllyNodeRef *root, ProllyHash *stash)
{
	ProllyHash	head;
	StashEntry *e = palloc(sizeof(StashEntry));
	char	   *branch = branch_ops()->current();
	List	   *entries;

	if (!copies_resolve("HEAD", &head))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the current branch has no commit to stash changes against")));
	lock_refs();
	entries = stash_get(name);
	write_commit(&root->hash, &head, 1, prolly_commit_author(),
				 psprintf("WIP on %s", branch), stash);
	e->stash = *stash;
	e->branch = pstrdup(branch);
	stash_set(name, lcons(e, entries));
}

static void
copies_stashes(ProllyStashVisit visit, void *arg)
{
	ProllyNodeRef root;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	char		tag = TAG_STASH;
	ProllyKey	from = {(const uint8 *) &tag, 1};

	refs_init();
	if (!refs_root(&root))
		return;
	c = PROLLY_CALL(tree, cursor_open, &env, &root, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, &from, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v) &&
		   k.len >= 1 && k.data[0] == TAG_STASH)
	{
		char	   *name = pnstrdup((const char *) k.data + 1, k.len - 1);
		int			n = 0;
		ListCell   *lc;

		foreach(lc, stash_list((const char *) v.data, (const char *) v.data + v.len))
		{
			StashEntry *e = lfirst(lc);

			visit(name, n++, e->branch, &e->stash, arg);
		}
	}
	PROLLY_CALL(tree, cursor_close, c);
}

static bool
copies_stash_select(const char *name, int n, ProllyHash *stash)
{
	List	   *entries;

	lock_refs();
	entries = stash_get(name);
	if (n < 0 || n >= list_length(entries))
		return false;
	*stash = ((StashEntry *) list_nth(entries, n))->stash;
	return true;
}

static bool
copies_stash_drop(const char *name, int n)
{
	List	   *entries;

	lock_refs();
	entries = stash_get(name);
	if (n < 0 || n >= list_length(entries))
		return false;
	stash_set(name, list_delete_nth_cell(entries, n));
	return true;
}

static void
copies_stash_clear(const char *name)
{
	lock_refs();
	if (stash_get(name) != NIL)
		stash_set(name, NIL);
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
	.read_working_set = copies_read_working_set,
	.read_definitions = copies_read_definitions,
	.working_changes = copies_working_changes,
	.publish_definitions = copies_publish_definitions,
	.fold_own = copies_fold_own,
	.checkpoint_unless_moved = copies_checkpoint_unless_moved,
	.moved_databases = branch_generations_moved_databases,
	.moved_branches = branch_generations_moved,
	.saw_branch = branch_generations_saw,
	.saw_database = branch_generations_saw_database,
	.reset = copies_reset,
	.merge_state = copies_merge_state,
	.set_merge_state = copies_set_merge_state,
	.hold = copies_hold,
	.lock = lock_refs,
	.walk = copies_walk,
	.checkpoint = copies_checkpoint,
	.set_head = copies_set_head,
	.violations = copies_violations,
	.set_violations = copies_set_violations,
	.walk_commit = copies_walk_commit,
	.remote_set = copies_remote_set,
	.remotes = copies_remotes,
	.tracking_set = copies_tracking_set,
	.trackings = copies_trackings,
	.upstream_set = copies_upstream_set,
	.upstream = copies_upstream,
	.branch_move = copies_branch_move,
	.config_get = copies_config_get,
	.config_set = copies_config_set,
	.open = refs_init,
	.commit_rows = copies_commit_rows,
	.branch_rename = copies_branch_rename,
	.root_refs = copies_root_refs,
	.stash_push = copies_stash_push,
	.stashes = copies_stashes,
	.stash_select = copies_stash_select,
	.stash_drop = copies_stash_drop,
	.stash_clear = copies_stash_clear,
	.shmem_size = branch_generations_shmem_size,
	.shmem_init = branch_generations_shmem_init,
	.note_write = branch_generations_note_write,
	.forget_folds = branch_generations_forget,
	.prepared_ended = branch_generations_prepared_ended,
	.note_relation = branch_generations_note_relation,
	.fold_mark = branch_fold_mark,
	.written_since = branch_written_since,
};
