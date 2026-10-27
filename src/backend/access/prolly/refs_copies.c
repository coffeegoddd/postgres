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
 *	'B' name	-> the branch's head commit, then its root, if it was
 *				   known when the head was set (hash length, hash each)
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
 *				   FullTransactionId, native byte order), then its head
 *				   log (see "Head logs"): the count of moves not yet in
 *				   its tree (a byte, its high bit set if the tree has
 *				   any), the tree's root, count of moves and last move's
 *				   date if so (as a remote-tracking branch's log's entry
 *				   holds them), then each such move's date (8 bytes
 *				   big-endian: so many bytes whatever the time between
 *				   moves) and commit (its hash's length and the hash's
 *				   bytes complemented), but the last move's, the head's
 *	'Y' name	-> the transaction that last moved its baseline (the same)
 *	'C' name	-> a database setting's value (text)
 *	'N' name	-> the head's tables, as the commit that made it of the
 *				   working set named them: the head's hash, then a line
 *				   per table (its OID, owner and qualified name, as a record's
 *				   lines are)
 *	'H' name	-> the head's other relations with privileges of their own
 *				   (views, materialized views, sequences and foreign
 *				   tables), as 'N' has its tables: the head's hash,
 *				   then a line for each (as a record's lines are), none written
 *				   when there are none; a merge of the branch, or a pick
 *				   of its head, takes their privileges
 *	'Q' name	-> the branch's merge's conflicted tables' names, as the
 *				   merge recorded them: each table's OID, then its name's
 *				   length (a varint) and bytes; and the tables in schema
 *				   conflicts, kept as the branch has them, each as OID 0
 *				   and its names so: ours', base's and theirs', NUL
 *				   between them
 *	'Z' name	-> a list of stashes, newest first: each its commit (hash
 *				   length, hash), then its branch's name (a varint length,
 *				   then the bytes), then the maker record of the role that
 *				   pushed it (as 'A''s)
 *	'I' name	-> the working set's identity anchor (the canonicalizer's
 *				   identify()), as of a head: the head's hash in hex (none
 *				   for a branch without commits), a newline, then the anchor
 *	'K' name	-> the height of the oldest of the commits below not
 *				   retired (an int64, native byte order; -1 if none or not
 *				   known: a move's nearest root is looked for no lower),
 *				   then the commits the branch's pages are kept as of, newest
 *				   first (see "Kept pages"): each the transaction that
 *				   retired it (a FullTransactionId, native byte order; 0
 *				   while it is kept), the transaction that kept it (the
 *				   same), when it was kept (a TimestampTz, the same), its
 *				   id and the branch's
 *				   baseline then (hash length, hash each), the count of
 *				   tables (a varint), and for each its qualified name (a
 *				   varint length, then the bytes), its data tree at the
 *				   commit (hash length, hash), the count of its
 *				   relations (a varint) and each relation's OID,
 *				   tablespace, file number, captured root (8 bytes), and
 *				   oldest unfrozen transaction and multixact ids (4 bytes
 *				   each but the snapshot, native byte order)
 *	'k' commit name -> nothing: branch name keeps pages as of the commit
 *				   (hash length, hash), an entry of its 'K' not retired
 *	's' data commit name -> nothing: and that commit's kept table has the
 *				   data tree data (hash length, hash, each)
 *	'r' root kind name -> nothing: the branch's head's root (kind 'h', as
 *				   'B' has it) or its baseline's fold's (kind 'w', as 'W'
 *				   has it) is root
 *				   (hash length, hash), for a first use's nearest branch
 *	'f' folded NUL name -> nothing: the branch's name case folded
 *				   (ASCII) is folded (with a head or not), for a new name
 *				   that differs from it only by case to be refused
 * Each index is changed with the entries it indexes, in the same
 * transaction, so it commits or rolls back with them.
 *	'F'			-> the records' tree's root (hash length, hash): a tree,
 *				   apart from the refs, of the relations each commit was
 *				   made from, under the commit's id (its hash's bytes), as
 *				   this database made it (commits fetched, cloned or pushed
 *				   here have none): a line for each prolly table, partitioned
 *				   table and materialized view, and for each other object
 *				   a commit may hold (the canonicalizer's
 *				   prolly_owned_objects(): views, sequences, foreign
 *				   tables, routines, types, schemas, collations, each
 *				   named as commits' items name it): its OID and its owner's
 *				   (10 digits each, a space after each), its kind (its
 *				   relkind, then a space), its owner's name (escaped as
 *				   the name is, and a space as a backslash and 's'; a
 *				   space after), and its qualified name (a backslash in it
 *				   written as two, a newline as a backslash and 'n'); the
 *				   routines and types last, by reference: a line of '@'
 *				   and the hex SHA-256 of their lines (none if there are
 *				   none), held under 'J'
 *	'J' hash	-> the lines of a record's routines and types (as a record's
 *				   lines are, sorted), under their content's hash in hex,
 *				   written once and named by every record listing them
 *	'D' name	-> the branch's merge's table identity conflicts: tables the
 *				   merge kept as the branch has them, as theirs is another
 *				   relation of the name: the local branch merged, if
 *				   theirs was one's head (a varint length, then the bytes;
 *				   0: none), the privileges of the branch's relations a
 *				   resolution with theirs dropped (the same: lines of
 *				   kind, name, column and ACL), then each table's relation here and theirs
 *				   (OIDs, 4 bytes each, big-endian), the inheritance or
 *				   partition tree it was kept with (a varint), its name
 *				   here and in theirs (a varint length, then the bytes,
 *				   each)
 *	'A' name	-> the maker record of the role that made the branch,
 *				   which owns it: the role's OID, then the id its shared
 *				   dependency (pg_shdepend, RefsBranchClassId) names the
 *				   branch by (native byte order each); main has none,
 *				   being the database owner's
 *	'G' name	-> the role that made the tag here (the same, its
 *				   dependency's RefsTagClassId): none for a tag fetched or
 *				   pushed
 *	'!'			-> the refs' format (REFS_FORMAT, text): refs without it, or
 *				   of another, are refused
 *	'l' remote NUL branch
 *				-> a remote-tracking branch's head log (see "Head logs"):
 *				   its tree's root (hash length, hash; 0: none yet), the
 *				   count of moves in it and the date of its last (8 bytes
 *				   big-endian each)
 *	'S' space key
 *				-> an entry another component keeps here (side_put()), in
 *				   its space (a byte): the history window's horizon; none
 *				   names a chunk
 *	'T' name	-> a tag: its tag object, then that tag's commit (hash
 *				   length, hash each; the commit kept here too, so that
 *				   resolving a tag, or reading an old root's refs, reads
 *				   no tag object)
 *	'P' name	-> a pin: a commit kept for a reader outside the refs (a
 *				   snapshot mount), named by it (hash length, hash), the
 *				   count (a varint) and roots (hash length, hash each) of
 *				   the trees it keeps beside it, then the reader's own
 *				   record of it
 *	'E' name	-> the branch's rebase in progress: the branch rebased (a
 *				   varint length, then the bytes), its head then and the
 *				   commit replayed onto (hash length, hash each), a flags
 *				   byte (1: keep commits that become empty), the order of
 *				   the step last begun (a varint length, then the bytes; 0:
 *				   none), the count of steps (a varint), then each step's
 *				   order and action (varint length, bytes), commit (hash
 *				   length, hash) and message (varint length, bytes)
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
 *	"identity"	-> the identity of its root's tables and columns, which
 *				   their renames keep (the canonicalizer's identify()):
 *				   its identity record; none if every object has its own
 *				   name's
 *	"message"	-> the message
 *	"parents"	-> the parents' ids, each a length byte and the hash
 *	"root"		-> the commit's root (length byte, hash): the canonical
 *				   form of the branch's versioned schemas, folded
 *	"time"		-> microseconds since the Postgres epoch, 8 bytes big-endian
 *
 * A tag object is a tree of its own too, and its id that tree's root hash,
 * so a tag's id is the same in every database it is cloned, pushed or
 * fetched to; a commit has a "root" item, a tag object none:
 *
 *	"commit"	-> the tagged commit (length byte, hash)
 *	"message"	-> the tag's message
 *	"tagger"	-> who made it
 *	"time"		-> when, as a commit's
 *
 * The chunks a commit's items and the refs' entries name (its dependencies,
 * parents and root; a tag's commit; a branch's head, baseline and working
 * set, a merge's commits, a remote branch's head, a tag's object and
 * commit, a kept commit and its baseline, a pin's commit) are declared to the tree as the
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
 * names its head's tables had, its merge state and violations, the
 * commits its pages are kept as of) is made
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

#include <math.h>

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
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_branch_d.h"
#include "catalog/pg_class.h"
#include "commands/trigger.h"
#include "common/cryptohash.h"
#include "common/hashfn.h"
#include "utils/injection_point.h"
#include "common/sha2.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "catalog/partition.h"
#include "refs_generations.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/lock.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/guc.h"
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
#define TAG_PRIVILEGED	'H'	/* and its other relations with privileges */
#define TAG_MERGE_NAMES	'Q'	/* a merge's conflicted tables' names then */
#define TAG_STASH		'Z'	/* a list of stashes, by name */
#define TAG_IDENTITY	'I'	/* the working set's identity anchor */
#define TAG_TAG			'T'	/* a tag, by name */
#define TAG_REBASE		'E'	/* a branch's rebase in progress */
#define TAG_KEPT		'K'	/* the commits a branch's pages are kept as of */
#define TAG_PIN			'P'	/* a commit pinned for a reader, by name */
#define TAG_CREATOR		'A'	/* the role that made a branch */
#define TAG_TAG_CREATOR	'G'	/* and a tag */
#define TAG_RECORDS		'F'	/* the relations commits were made from: a tree's root */
#define TAG_OWNED		'J'	/* routines and types such records list */
#define TAG_IDENTITY_CONFLICTS 'D'	/* a branch's merge's table identity conflicts */
#define TAG_FORMAT		'!'	/* the refs' format, REFS_FORMAT */
#define TAG_TRACKING_LOG 'l'	/* a remote-tracking branch's head log */
#define TAG_SIDE		'S'	/* another component's entries, by space */
#define TAIL_TREE		0x80	/* in a mover entry: its head log's tree follows */
/*
 * Indexes of other entries, each kept with them, so that a lookup by what
 * they hold is a seek of the refs, not a read of every branch's entries;
 * each key ends with the branch's name.
 */
#define TAG_KEPT_OF		'k'	/* a commit's branches keeping its pages */
#define TAG_KEPT_TABLE	's'	/* a table's kept commits holding it */
#define TAG_ROOT_OF		'r'	/* a root's branches with a head or baseline at it */
#define TAG_FOLDED		'f'	/* a branch's name, case folded */

/*
 * The refs tree's format, kept in it: a tree of another (an older build's)
 * is refused, never read.  2: tags; 3: rebases; 4: kept pages; 5: kept
 * pages retired; 8: a merge's tables in schema conflicts, in its merge
 * names entry; 9: kept pages' keeping transaction; 10: pins; 11: the
 * trees a pin keeps; 12: a schema-conflicted table's names on each side;
 * 13: branches', tags' and stashes' makers; 14: a maker record's shared
 * dependency id, after its role; 15: the relations commits were made from
 * (names escaped), and relations' equivalence classes under their own tag;
 * 16: a branch's head's other relations with privileges of their own, and
 * every record's relation names quoted as with quote_all_identifiers off;
 * 17: a merge's table identity conflicts under their own tag, each
 * record's relations' owners, partitioned tables in commits' records, and
 * no classes (a move keeps a table's relation, by OID); 19: each kept
 * table's data tree, a head's root after the head, and the indexes of kept commits by commit and table,
 * of branches by their heads' and baselines' roots, and of branches' names
 * case folded; 20: head logs, and a head's moves not yet in its log
 * after the transaction that moved it; 21: a branch's kept list may name
 * the storage a restore replaced (a retired entry whose relations'
 * snapshot is BRANCH_KEPT_STORAGE), dropped once no snapshot is older than
 * it; 22: other components' entries (the history horizon).
 * 23: kept relations' pages by roots their page stores keep (not
 * snapshots their storage keeps), and each kept commit's time.
 */
#define REFS_FORMAT		"23"

/* A maker record's length: the role, then its dependency's id (each an OID) */
#define MAKER_RECORD_LEN	(2 * sizeof(Oid))

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
struct ScopedPending;

typedef struct ScopedSave
{
	int			level;
	bool		scoped;
	bool		whole;
	ProllyHash	base;
	ProllyHash	mine;
	List	   *pending;		/* (scoped_pending's copy) */
	MemoryContext cxt;			/* (the save's, its copy's too) */
	int			nrecords;		/* of scoped_records */
} ScopedSave;

static bool scoped = false;
static bool whole = false;		/* the transaction holds every ref */
static ProllyHash scoped_base;
static ProllyHash scoped_mine;
static bool published = false;
static List *scoped_saved = NIL;	/* ScopedSave *, innermost last */

/*
 * The scoped changes' puts and deletes not yet applied to scoped_mine (in
 * TopTransactionContext): applied together, in one edit of the tree, when
 * the tree is next read whole (refs_root()), rebuilt, saved for a
 * subtransaction or published.  A commit puts several entries (its head,
 * its record, its names, its movers); applied one at a time each would
 * write its own path from the root down, a path that grows with the refs
 * (every commit's record is in them), so a commit's work would grow with
 * the commits before it.  refs_get() reads an entry pending first.
 */
typedef struct ScopedPending
{
	char	   *key;
	int			keylen;
	char	   *value;			/* (NULL: a delete) */
	int			len;
} ScopedPending;

static List *scoped_pending = NIL;	/* ScopedPending *, by key */
static MemoryContext pending_cxt = NULL;	/* (its entries', emptied as
											 * they are applied) */
static void flush_scoped(void);

/* The context pending scoped changes are kept in, under the transaction's */
static MemoryContext
pending_context(void)
{
	if (pending_cxt == NULL)
		pending_cxt = AllocSetContextCreate(TopTransactionContext,
											"prolly scoped refs pending",
											ALLOCSET_SMALL_SIZES);
	return pending_cxt;
}

/* A copy of pending scoped changes, in the current memory context */
static List *
pending_copy(List *pending)
{
	List	   *out = NIL;

	foreach_ptr(ScopedPending, sp, pending)
	{
		ScopedPending *c = palloc(sizeof(ScopedPending));

		c->key = palloc(Max(sp->keylen, 1));
		memcpy(c->key, sp->key, sp->keylen);
		c->keylen = sp->keylen;
		c->value = NULL;
		if (sp->value)
		{
			c->value = palloc(Max(sp->len, 1));
			memcpy(c->value, sp->value, sp->len);
		}
		c->len = sp->len;
		out = lappend(out, c);
	}
	return out;
}

/*
 * The commits (ProllyHash *, in TopTransactionContext) whose relation
 * records the scoped changes put, put again whenever they are rebuilt on a
 * root another moved: a record equal to one the base had is no change of
 * the base's, and a step after a collection that forgot it meanwhile (its
 * commit gone, then made again) would otherwise take it with it.
 */
typedef struct ScopedEdits ScopedEdits;

static List *scoped_records = NIL;

/* The records' tree's environment: its values name no chunks. */
static ProllyTreeEnv records_env;
static ProllyTreeEnv log_env;	/* head logs' trees: naming no chunks */

static void records_under(const ProllyNodeRef *refs, ProllyNodeRef *records,
						  bool *any);
static void empty_records(ProllyNodeRef *records);
static void records_rebase(const ProllyNodeRef *from, const ProllyNodeRef *to,
						   const ProllyHash *cur, ScopedEdits *se);

static void put_records_again(const ProllyNodeRef *mine, ScopedEdits *se);

/*
 * The branches whose merge states the transaction set (names, in
 * TopTransactionContext): checked when it commits or prepares, whichever
 * branch it is on then.
 */
static List *merge_changed = NIL;

static void refs_xact_callback(XactEvent event, void *arg);
static void forget_read_caches(void);
static void refs_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
								  SubTransactionId parentSubid, void *arg);
static void publish_scoped(void);
static void head_log_note(const char *name, const ProllyHash *was,
						  const ProllyHash *head, StringInfo mover);
static void tracking_log_note(const char *remote, const char *branch,
							  const ProllyHash *commit);
static bool merge_state_of(const char *branch, ProllyMergeState *state);
static void merge_tables_left(const char *branch, ProllyMergeState *state,
							  char **name);
static List *violations_left(const char *branch, List *violations,
							 char **name);
static Oid	branch_oid(const char *name);
static char *branch_name_of(Oid branch);
static void merge_key(StringInfo key, const char *branch);
static void merge_names_put(const char *branch, const ProllyMergeState *state);
static void merge_names_get(const char *branch, ProllyMergeState *state);
static void identity_conflicts_put(const char *branch,
								   const ProllyMergeState *state);
static void identity_conflicts_get(const char *branch,
								   ProllyMergeState *state);
static List *violations_of(const char *branch);
static void violations_key(StringInfo key, const char *branch);
static List *copies_violations(void);
static bool tracking_head(const char *refname, ProllyHash *commit_id);
static void kept_declare(const char *p, const char *end, StringInfo refs);
static bool tag_entry(const char *name, ProllyHash *tag, ProllyHash *commit);
static bool copies_tag_get(const ProllyHash *tag, ProllyTagInfo *info);
static bool upstream_head(const char *branch, ProllyHash *commit_id);
static void copies_set_violations(List *violations);
static void copies_visit_violations(ProllyViolationVisit visit, void *arg);
static void copies_add_violations(ProllyViolation *const *violations, int n);
static void violations_clear(const char *branch);
static void violations_move(const char *from, const char *to);
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

static ProllyHash *
copy_hash_of(const ProllyHash *h)
{
	ProllyHash *c = palloc(sizeof(ProllyHash));

	*c = *h;
	return c;
}

/* A varint-length string at *p (palloc'd); NULL if it doesn't fit. */
static char *
get_short_text(const char **p, const char *end)
{
	uint64		len = prolly_get_varint(p, end);
	char	   *s;

	if (len > (uint64) (end - *p))
		return NULL;
	s = pnstrdup(*p, len);
	*p += len;
	return s;
}

/*
 * A rebase state's entry, read; false if it is malformed (its hashes are
 * still read as get_hash() reads them, which errors on a bad one).
 */
static bool
rebase_decode(const char *p, const char *end, ProllyRebaseState *state)
{
	uint64		n;

	memset(state, 0, sizeof(*state));
	if ((state->branch = get_short_text(&p, end)) == NULL)
		return false;
	p = get_hash(p, end, &state->head);
	p = get_hash(p, end, &state->onto);
	if (p >= end)
		return false;
	state->keep_empty = (*p++ & 1) != 0;
	if ((state->last_step = get_short_text(&p, end)) == NULL)
		return false;
	if (state->last_step[0] == '\0')
		state->last_step = NULL;
	n = prolly_get_varint(&p, end);
	if (n > (uint64) (end - p))
		return false;
	state->steps = palloc0(sizeof(ProllyRebaseStep) * Max(n, 1));
	for (uint64 i = 0; i < n; i++)
	{
		ProllyRebaseStep *s = &state->steps[i];

		if ((s->order = get_short_text(&p, end)) == NULL ||
			(s->action = get_short_text(&p, end)) == NULL)
			return false;
		p = get_hash(p, end, &s->commit);
		if ((s->message = get_short_text(&p, end)) == NULL)
			return false;
		state->nsteps++;
	}
	return true;
}

/*
 * A commit's items: its dependencies, parents and root name chunks; a tag
 * object's: its commit.
 */
static void
commit_chunk_refs(const ProllyKey *key, const ProllyValue *value, void *arg,
				  StringInfo refs)
{
	const char *p = (const char *) value->data;
	const char *end = p + value->len;

#define KEY_IS(name) \
	(key->len == strlen(name) && memcmp(key->data, name, key->len) == 0)
	if (KEY_IS("dependencies") || KEY_IS("parents") || KEY_IS("root") ||
		KEY_IS("commit"))
		(void) declare_hashes(p, end, INT_MAX, refs);
#undef KEY_IS
}

/*
 * The refs' entries: a branch's head, a remote branch's, a baseline and the
 * working set its fold made, a merge's commits (theirs and the base; a
 * revert's series head and the commits it has left), each stash's commit,
 * and each tag's object and commit.
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
		case TAG_TRACKING:
		case TAG_RECORDS:
		case TAG_TRACKING_LOG:	/* (its tree: whose values name nothing) */
			(void) declare_hashes(p, end, 1, refs);
			break;
		case TAG_MOVER:			/* (after the transaction, its head log's tree) */
			if (end - p > sizeof(FullTransactionId) &&
				(p[sizeof(FullTransactionId)] & TAIL_TREE))
				(void) declare_hashes(p + sizeof(FullTransactionId) + 1, end, 1,
									  refs);
			break;
		case TAG_BRANCH:			/* (the head, and its root if recorded) */
		case TAG_BASELINE:
		case TAG_TAG:
			(void) declare_hashes(p, end, 2, refs);
			break;
		case TAG_PIN:
			{
				uint64		n;

				p = declare_hashes(p, end, 1, refs);
				n = prolly_get_varint(&p, end);
				(void) declare_hashes(p, end, (int) Min(n, INT_MAX), refs);
			}
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
		case TAG_REBASE:
			{
				ProllyRebaseState rs;

				if (!rebase_decode(p, end, &rs))
					break;
				PROLLY_CALL(tree, value_reference, &rs.head, 0, refs);
				PROLLY_CALL(tree, value_reference, &rs.onto, 0, refs);
				for (int i = 0; i < rs.nsteps; i++)
					PROLLY_CALL(tree, value_reference, &rs.steps[i].commit, 0, refs);
			}
			break;
		case TAG_KEPT:
			kept_declare(p, end, refs);
			break;
		case TAG_STASH:
			/* (each entry: its commit, its branch's name, its maker) */
			while (p < end)
			{
				uint64		blen;

				p = declare_hashes(p, end, 1, refs);
				blen = prolly_get_varint(&p, end);
				if (blen + MAKER_RECORD_LEN > (uint64) (end - p))
					break;
				p += blen + MAKER_RECORD_LEN;
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
	/* (counted apart too: see pgstat_prolly_refs_tree_counters()) */
	env.counters = pgstat_prolly_refs_tree_counters();
	commit_env = env;
	records_env = env;
	log_env = env;
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
		Oid			branch = lfirst_oid(lc);
		char	   *name;

		if (branch == MyBranch)
			name = branch_ops()->current();
		else
			name = branch_name_of(branch);
		if (name != NULL && !name_listed(names, name))
			names = lappend(names, name);
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
		if (ms.nschema > 0)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("cannot %s a transaction with unresolved merge conflicts", what),
					 errdetail("Table %s has a schema conflict on branch \"%s\".",
							   ms.schema_tables[0], branch),
					 errhint("Abort the merge, make the definitions agree on one side, and merge again; or set dolt.allow_commit_conflicts to keep them.")));
		if (ms.nidentity > 0)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("cannot %s a transaction with unresolved merge conflicts", what),
					 errdetail("Table %s has a table identity conflict on branch \"%s\".",
							   ms.identity[0].name, branch),
					 errhint("Resolve it with dolt_conflicts_resolve('--ours') or ('--theirs') first, or set dolt.allow_commit_conflicts to keep it.")));
		v = violations_left(branch, violations_of(branch), &name);
		if (v != NIL)
		{
			const char *type = ((ProllyViolation *) linitial(v))->type;

			/* ("an exclusion constraint violation", but "a unique index ...") */
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("cannot %s a transaction with constraint violations", what),
					 errdetail("Table \"%s\" has a row with %s %s violation on branch \"%s\".",
							   name, type[0] == 'e' ? "an" : "a",
							   type, branch),
					 errhint("Fix the rows and check them again with dolt_verify_constraints(), or set dolt.allow_commit_conflicts to keep them.")));
		}
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
			scoped_records = NIL;
			scoped_pending = NIL;
			pending_cxt = NULL; /* (TopTransactionContext's: gone) */
			forget_read_caches();
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

/*
 * The branch kept_get() read last, and the index entries of its kept
 * commits then (in kept_read_cxt): kept_put() of it after takes them,
 * the caller having changed the list read in place.  Forgotten by every
 * kept_put(), so a write in between makes it read the entry again.
 */
static MemoryContext kept_read_cxt = NULL;
static char *kept_read_name = NULL;
static List *kept_read_keys = NIL;

/*
 * The branch branch_set_baseline() wrote, or a read of its 'W' found, last,
 * while the transaction held the branch (so no other session changed it
 * since), and the root of its baseline's fold then (have_root false: none):
 * the next baseline set of it changes the root's index from there, without
 * reading 'W' again.  In TopMemoryContext.
 */
static char *baseline_seen_name = NULL;
static bool baseline_seen_have;
static ProllyNodeRef baseline_seen_root;

/*
 * Forget what was read of the refs, as a transaction ends or a
 * subtransaction rolls back: another session may change them before this
 * one reads them next, or the rollback undid what was written.
 */
static bool branch_held(const char *name, int len);

static void
baseline_seen_set(const char *name, bool have_root, const ProllyHash *root)
{
	if (baseline_seen_name == NULL || strcmp(baseline_seen_name, name) != 0)
	{
		if (baseline_seen_name != NULL)
			pfree(baseline_seen_name);
		baseline_seen_name = MemoryContextStrdup(TopMemoryContext, name);
	}
	baseline_seen_have = have_root;
	if (have_root)
		ref_of(root, &baseline_seen_root);
}

/*
 * A 'W' value's fold root, noted as seen (the entry found or not): only
 * while the transaction holds the branch, so that no other session
 * changes it before the next baseline set reads it from here (a read
 * without the lock, in one READ COMMITTED statement, may be older than
 * the next statement's).
 */
static void
baseline_seen_value(const char *name, const StringInfoData *v, bool found)
{
	ProllyHash	h;
	const char *p;

	if (!branch_held(name, strlen(name)))
		return;
	if (!found)
	{
		baseline_seen_set(name, false, NULL);
		return;
	}
	p = get_hash(v->data, v->data + v->len, &h);
	if (p < v->data + v->len)
	{
		get_hash(p, v->data + v->len, &h);
		baseline_seen_set(name, h.len > 0, &h);
	}
	else
		baseline_seen_set(name, false, NULL);
}

static void
forget_read_caches(void)
{
	kept_read_name = NULL;
	if (baseline_seen_name != NULL)
		pfree(baseline_seen_name);
	baseline_seen_name = NULL;
}

/* ------------------------------------------------------------------------
 * The refs tree
 * ------------------------------------------------------------------------
 */

/*
 * Refuse a refs tree of another format than REFS_FORMAT (an older one has
 * no TAG_FORMAT entry): this build reads only its own.  Checked once a
 * backend (a backend serves one database): every root after the one checked
 * was made from it, by builds that keep the entry.
 */
static void
check_refs_format(const ProllyNodeRef *root)
{
	static bool checked = false;
	char		k = TAG_FORMAT;
	ProllyKey	key = {(const uint8 *) &k, 1};
	StringInfoData v;

	if (checked)
		return;
	initStringInfo(&v);
	if (!PROLLY_CALL(tree, get, &env, root, &key, &v))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's prolly refs are of an older format than %s",
						REFS_FORMAT),
				 errdetail("This build reads only refs of format %s.", REFS_FORMAT)));
	if (strcmp(v.data, REFS_FORMAT) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's prolly refs are of format %s, not %s",
						v.data, REFS_FORMAT),
				 errdetail("This build reads only refs of format %s.", REFS_FORMAT)));
	pfree(v.data);
	checked = true;
}

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
	{
		MemoryContext cxt = AllocSetContextCreate(TopTransactionContext,
												  "prolly scoped refs saved",
												  ALLOCSET_SMALL_SIZES);

		old = MemoryContextSwitchTo(cxt);
		save = palloc(sizeof(ScopedSave));
		save->cxt = cxt;
	}
	save->level = level;
	save->scoped = scoped;
	save->whole = whole;
	save->base = scoped_base;
	save->mine = scoped_mine;
	/*
	 * (what is pending is the parent's, saved as it is: applied in the
	 * subtransaction, the nodes written would go with it, were it rolled
	 * back)
	 */
	save->pending = pending_copy(scoped_pending);
	save->nrecords = list_length(scoped_records);
	MemoryContextSwitchTo(TopTransactionContext);
	scoped_saved = lappend(scoped_saved, save);
	MemoryContextSwitchTo(old);
}

static void
refs_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					  SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();
	ScopedSave *last;

	/* (what a rolled-back subtransaction read or wrote is no longer so) */
	if (event == SUBXACT_EVENT_ABORT_SUB)
		forget_read_caches();
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
		/* (the saved copy, as the pending changes now) */
		if (pending_cxt != NULL)
			MemoryContextReset(pending_cxt);
		{
			MemoryContext old = MemoryContextSwitchTo(pending_context());

			scoped_pending = pending_copy(last->pending);
			MemoryContextSwitchTo(old);
		}
		scoped_records = list_truncate(scoped_records, last->nrecords);
		scoped_saved = list_delete_last(scoped_saved);
		MemoryContextDelete(last->cxt);
	}
	else if (event == SUBXACT_EVENT_COMMIT_SUB)
	{
		/* the parent's saved state, if it has one, is the older */
		if (level - 1 <= 1 ||
			(list_length(scoped_saved) > 1 &&
			 ((ScopedSave *) list_nth(scoped_saved,
									  list_length(scoped_saved) - 2))->level == level - 1))
		{
			scoped_saved = list_delete_last(scoped_saved);
			MemoryContextDelete(last->cxt);
		}
		else
			last->level = level - 1;
	}
}

struct ScopedEdits
{
	ProllyEdit *edits;
	int			n;
	int			max;
};

static bool
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
	return true;
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
	/* (what is pending applies to mine: rebuilt with it) */
	flush_scoped();
	ref_of(&scoped_base, &from);
	ref_of(&scoped_mine, &to);
	PROLLY_CALL(tree, diff, &env, &from, &to, collect_edit, &se);
	records_rebase(&from, &to, cur, &se);
	put_records_again(&to, &se);
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

static int
edit_cmp(const void *a, const void *b)
{
	const ProllyEdit *x = a;
	const ProllyEdit *y = b;
	int			c = memcmp(x->key.data, y->key.data, Min(x->key.len, y->key.len));

	return c != 0 ? c : (x->key.len > y->key.len) - (x->key.len < y->key.len);
}

/* Whether se has an edit of key (len bytes); its value into *value if so */
static bool
edit_of(ScopedEdits *se, const char *key, int len, ProllyValue *value)
{
	for (int i = 0; i < se->n; i++)
		if (se->edits[i].key.len == len &&
			memcmp(se->edits[i].key.data, key, len) == 0)
		{
			if (value)
				*value = se->edits[i].value;
			return true;
		}
	return false;
}

/*
 * A copy of a key of len bytes, as it is: binary (a commit's id may hold a
 * zero byte), never cut at one as pnstrdup() would cut it.
 */
static char *
key_copy(const char *key, int len)
{
	char	   *k = palloc(Max(len, 1));

	memcpy(k, key, len);
	return k;
}

/*
 * Add to se a put of key (len bytes) as mine has it, unless se has an edit
 * of it already or mine hasn't it: true if added.
 */
static bool
put_again(const ProllyNodeRef *mine, ScopedEdits *se, const char *key, int len)
{
	ProllyKey	k = {(const uint8 *) key, len};
	StringInfoData v;
	ProllyEdit *e;

	if (edit_of(se, key, len, NULL))
		return false;
	initStringInfo(&v);
	if (!PROLLY_CALL(tree, get, &env, mine, &k, &v))
	{
		pfree(v.data);
		return false;
	}
	if (se->n == se->max)
	{
		se->max = Max(8, se->max * 2);
		se->edits = se->edits ? repalloc_array(se->edits, ProllyEdit, se->max) :
			palloc_array(ProllyEdit, se->max);
	}
	e = &se->edits[se->n++];
	e->key.data = (const uint8 *) key_copy(key, len);
	e->key.len = len;
	e->is_delete = false;
	e->value.data = (const uint8 *) v.data;
	e->value.len = v.len;
	return true;
}

static bool owned_reference(const char *value, int len, char *hash);

/*
 * The routines' and types' entry (TAG_OWNED) each record of a branch's
 * head's tables the scoped changes put names, added to the edits rebuilt
 * on another root where the diff hasn't it: one found stored already
 * wasn't put, and a collection since may have forgotten it, no record then
 * naming it (records_rebase() does the same for commits' records); the
 * edits in key order.
 */
static void
put_records_again(const ProllyNodeRef *mine, ScopedEdits *se)
{
	bool		added = false;
	int			n;

	n = se->n;
	for (int i = 0; i < n; i++)
	{
		ProllyEdit *e = &se->edits[i];
		char		hash[2 * PG_SHA256_DIGEST_LENGTH + 1];
		char		key[1 + 2 * PG_SHA256_DIGEST_LENGTH + 1];

		if (e->is_delete || e->key.len < 1 ||
			e->key.data[0] != TAG_NAMES ||
			!owned_reference((const char *) e->value.data, e->value.len, hash))
			continue;
		snprintf(key, sizeof(key), "%c%s", TAG_OWNED, hash);
		added |= put_again(mine, se, key, strlen(key));
	}
	if (added)
		qsort(se->edits, se->n, sizeof(ProllyEdit), edit_cmp);
}

/*
 * Rebuilt on cur, the commits' records the scoped changes made (the records
 * tree's changes from the base's to mine's), with those of scoped_records
 * mine has put again where they aren't changes, made on cur's records: its
 * root in se in place of mine's (TAG_RECORDS is every commit's, changed by
 * others' commits meanwhile, unlike a branch's entries); and the routines'
 * and types' entry each record put names, as put_records_again() does.
 */
static void
records_rebase(const ProllyNodeRef *from, const ProllyNodeRef *to,
			   const ProllyHash *cur, ScopedEdits *se)
{
	char		tag = TAG_RECORDS;
	ScopedEdits re = {NULL, 0, 0};
	ProllyNodeRef onto;
	ProllyNodeRef base_records;
	ProllyNodeRef mine_records;
	ProllyNodeRef cur_records;
	ProllyNodeRef merged;
	bool		any;
	bool		base_any;
	bool		cur_any;
	HTAB	   *changed;
	int			at = -1;
	StringInfoData v;

	for (int i = 0; i < se->n; i++)
		if (se->edits[i].key.len == 1 && se->edits[i].key.data[0] == TAG_RECORDS)
			at = i;
	if (at < 0 && scoped_records == NIL)
		return;
	records_under(from, &base_records, &base_any);
	records_under(to, &mine_records, &any);
	if (!any)
		return;
	/* (a base before the first record: its records the empty tree) */
	if (!base_any)
		empty_records(&base_records);
	ref_of(cur, &onto);
	records_under(&onto, &cur_records, &cur_any);
	PROLLY_CALL(tree, diff, &records_env, &base_records, &mine_records,
				collect_edit, &re);
	/* (the commits whose records the diff has, by id) */
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(ProllyHash);
		ctl.entrysize = sizeof(ProllyHash);
		ctl.hcxt = CurrentMemoryContext;
		changed = hash_create("prolly records rebuilt", Max(re.n, 16), &ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		for (int i = 0; i < re.n; i++)
		{
			ProllyHash	h;

			memset(&h, 0, sizeof(h));
			h.len = Min(re.edits[i].key.len, PROLLY_HASH_MAX_LEN);
			memcpy(h.bytes, re.edits[i].key.data, h.len);
			(void) hash_search(changed, &h, HASH_ENTER, NULL);
		}
	}
	foreach_ptr(ProllyHash, commit, scoped_records)
	{
		ProllyKey	k = {commit->bytes, commit->len};
		ProllyHash	h;

		memset(&h, 0, sizeof(h));
		h.len = commit->len;
		memcpy(h.bytes, commit->bytes, commit->len);
		if (hash_search(changed, &h, HASH_FIND, NULL) == NULL)
		{
			StringInfoData rv;
			ProllyEdit *e;

			initStringInfo(&rv);
			if (!PROLLY_CALL(tree, get, &records_env, &mine_records, &k, &rv))
			{
				pfree(rv.data);
				continue;
			}
			if (re.n == re.max)
			{
				re.max = Max(8, re.max * 2);
				re.edits = re.edits ? repalloc_array(re.edits, ProllyEdit, re.max) :
					palloc_array(ProllyEdit, re.max);
			}
			e = &re.edits[re.n++];
			e->key.data = (const uint8 *) key_copy((const char *) commit->bytes, commit->len);
			e->key.len = commit->len;
			e->is_delete = false;
			e->value.data = (const uint8 *) rv.data;
			e->value.len = rv.len;
		}
	}
	hash_destroy(changed);
	if (re.n == 0)
	{
		/* (nothing of mine: cur's records stand) */
		if (at >= 0)
		{
			se->edits[at] = se->edits[--se->n];
			qsort(se->edits, se->n, sizeof(ProllyEdit), edit_cmp);
		}
		return;
	}
	qsort(re.edits, re.n, sizeof(ProllyEdit), edit_cmp);
	if (!cur_any)
		empty_records(&cur_records);
	PROLLY_CALL(tree, apply, &records_env, &cur_records, re.edits, re.n, &merged);
	initStringInfo(&v);
	put_hash(&v, &merged.hash);
	if (at < 0)
	{
		if (se->n == se->max)
		{
			se->max = Max(8, se->max * 2);
			se->edits = se->edits ? repalloc_array(se->edits, ProllyEdit, se->max) :
				palloc_array(ProllyEdit, se->max);
		}
		at = se->n++;
		se->edits[at].key.data = (const uint8 *) pnstrdup(&tag, 1);
		se->edits[at].key.len = 1;
		se->edits[at].is_delete = false;
	}
	se->edits[at].value.data = (const uint8 *) v.data;
	se->edits[at].value.len = v.len;
	/* (the entries the records name, as put_records_again()'s) */
	for (int i = 0; i < re.n; i++)
	{
		char		hash[2 * PG_SHA256_DIGEST_LENGTH + 1];
		char		key[1 + 2 * PG_SHA256_DIGEST_LENGTH + 1];

		if (re.edits[i].is_delete ||
			!owned_reference((const char *) re.edits[i].value.data,
							 re.edits[i].value.len, hash))
			continue;
		snprintf(key, sizeof(key), "%c%s", TAG_OWNED, hash);
		(void) put_again(to, se, key, strlen(key));
	}
	qsort(se->edits, se->n, sizeof(ProllyEdit), edit_cmp);
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
	flush_scoped();
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

/*
 * ... but for the scoped changes pending (refs_get() reads those first).
 */
static bool
refs_root_unflushed(ProllyNodeRef *root)
{
	ProllyHash	h;

	PROLLY_CALL(env.chunk_store, root, env.chunks, &h);
	if (scoped)
	{
		rebase_scoped(&h);
		h = scoped_mine;
	}
	if (h.len == 0)
		return false;
	ref_of(&h, root);
	check_refs_format(root);
	return true;
}

/* The pending scoped change of key (len bytes), if any. */
static ScopedPending *
pending_of(const char *key, int keylen)
{
	foreach_ptr(ScopedPending, sp, scoped_pending)
		if (sp->keylen == keylen && memcmp(sp->key, key, keylen) == 0)
			return sp;
	return NULL;
}

static int
pending_cmp(const ListCell *a, const ListCell *b)
{
	const ScopedPending *x = lfirst(a);
	const ScopedPending *y = lfirst(b);
	int			c = memcmp(x->key, y->key, Min(x->keylen, y->keylen));

	return c != 0 ? c : (x->keylen > y->keylen) - (x->keylen < y->keylen);
}

/* Apply the pending scoped changes to scoped_mine, in one edit. */
static void
flush_scoped(void)
{
	ProllyEdit *edits;
	ProllyNodeRef root;
	ProllyNodeRef new_root;
	int			n = 0;

	if (scoped_pending == NIL)
		return;
	Assert(scoped);
	/* (the subtransaction's change of mine, undone if it is rolled back) */
	scoped_save();
	list_sort(scoped_pending, pending_cmp);
	edits = palloc_array(ProllyEdit, list_length(scoped_pending));
	foreach_ptr(ScopedPending, sp, scoped_pending)
	{
		edits[n].key.data = (const uint8 *) sp->key;
		edits[n].key.len = sp->keylen;
		edits[n].is_delete = (sp->value == NULL);
		edits[n].value.data = (const uint8 *) sp->value;
		edits[n].value.len = sp->len;
		n++;
	}
	ref_of(&scoped_mine, &root);
	PROLLY_CALL(tree, apply, &env, &root, edits, n, &new_root);
	scoped_mine = new_root.hash;
	pfree(edits);
	scoped_pending = NIL;
	MemoryContextReset(pending_cxt);
}

/* The refs tree's root, as the transaction sees it; false if none yet. */
static bool
refs_root(ProllyNodeRef *root)
{
	refs_init();
	flush_scoped();
	return refs_root_unflushed(root);
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

/* Let go of the lock try_lock_branch() held of branch name. */
static void
release_branch(const char *name)
{
	LOCKTAG		tag;

	branch_lock_tag(&tag, name, strlen(name));
	LockRelease(&tag, ExclusiveLock, false);
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
			char		k = TAG_FORMAT;
			ProllyKey	key = {(const uint8 *) &k, 1};
			ProllyValue v = {(const uint8 *) REFS_FORMAT, strlen(REFS_FORMAT)};

			/* (a new store's refs: only their format) */
			PROLLY_CALL(tree, build_add, b, &key, &v);
			PROLLY_CALL(tree, build_finish, b, &empty);
			next = empty.hash;
		}
		else
		{
			ProllyNodeRef r;

			ref_of(&cur, &r);
			check_refs_format(&r);
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

/*
 * The name of the branch key is one of the entries of (*len bytes), or
 * NULL if it is none of a branch's: after the tag, or for an index, after
 * what it is keyed by.
 */
static bool branch_entry_tag(char tag);

static const char *
entry_branch(const char *key, int keylen, int *len)
{
	const char *p = key + 1;
	const char *end = key + keylen;
	int			skip = 0;

	if (keylen < 2)
		return NULL;
	switch (key[0])
	{
		case TAG_KEPT_OF:
		case TAG_ROOT_OF:
			skip = 1;
			break;
		case TAG_KEPT_TABLE:
			skip = 2;
			break;
		case TAG_FOLDED:
			p = memchr(p, '\0', end - p);
			if (p == NULL)
				return NULL;
			p++;
			break;
		default:
			if (!branch_entry_tag(key[0]))
				return NULL;
			break;
	}
	for (int i = 0; i < skip; i++)
	{
		if (p >= end || end - p < 1 + (uint8) p[0])
			return NULL;
		p += 1 + (uint8) p[0];
	}
	/* (a root's index has the kind, head or baseline, before the name) */
	if (key[0] == TAG_ROOT_OF)
		p++;
	if (p >= end)
		return NULL;
	*len = end - p;
	return p;
}

/* Is tag that of an entry keyed by a branch's name alone, after the tag? */
static bool
branch_entry_tag(char tag)
{
	switch (tag)
	{
		case TAG_BRANCH:
		case TAG_MOVER:
		case TAG_BASELINE:
		case TAG_BASELINE_MOVER:
		case TAG_NAMES:
		case TAG_PRIVILEGED:
		case TAG_MERGE:
		case TAG_MERGE_NAMES:
		case TAG_IDENTITY_CONFLICTS:
		case TAG_VIOLATIONS:
		case TAG_IDENTITY:
		case TAG_REBASE:
		case TAG_KEPT:
		case TAG_CREATOR:
			return true;
		default:
			return false;
	}
}

/*
 * May the entry key be changed in the scoped changes: one of a branch the
 * transaction holds, or the relation record of a commit it makes (keyed by
 * the commit, which no other transaction writes), not having held every
 * ref nor begun to commit?
 */
static bool
scoped_key(const char *key, int keylen)
{
	const char *name;
	int			len;

	if (published || whole)
		return false;
	if ((keylen == 1 && key[0] == TAG_RECORDS) ||
		(keylen > 1 && key[0] == TAG_OWNED))
		return true;
	name = entry_branch(key, keylen, &len);
	return name != NULL && branch_held(name, len);
}

static bool
refs_get(const char *key, int keylen, StringInfo value)
{
	ProllyNodeRef root;
	ProllyKey	k = {(const uint8 *) key, keylen};
	ScopedPending *sp;

	refs_init();
	if (scoped && (sp = pending_of(key, keylen)) != NULL)
	{
		if (sp->value == NULL)
			return false;
		resetStringInfo(value);
		appendBinaryStringInfo(value, sp->value, sp->len);
		return true;
	}
	if (!refs_root_unflushed(&root))
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
			{
				MemoryContext old = MemoryContextSwitchTo(pending_context());
				ScopedPending *sp = pending_of(key, keylen);

				if (sp == NULL)
				{
					sp = palloc(sizeof(ScopedPending));
					sp->key = palloc(Max(keylen, 1));
					memcpy(sp->key, key, keylen);
					sp->keylen = keylen;
					scoped_pending = lappend(scoped_pending, sp);
				}
				else if (sp->value)
					pfree(sp->value);
				sp->value = NULL;
				if (value)
				{
					sp->value = palloc(Max(value->len, 1));
					memcpy(sp->value, value->data, value->len);
				}
				sp->len = value ? value->len : 0;
				MemoryContextSwitchTo(old);
			}
			return;
		}
	}
	lock_refs();

	/* (a branch's entries are changed only by a holder of its lock) */
	{
		int			len;
		const char *name = entry_branch(key, keylen, &len);

		if (name != NULL && !branch_held(name, len))
		{
			char	   *n = pnstrdup(name, len);

			lock_branch(n);
			pfree(n);
		}
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
 * A maker record: the role, and the id its shared dependency names the
 * object by (newMadeObjectId()).
 */
typedef struct MakerRecord
{
	Oid			role;
	Oid			id;
} MakerRecord;

/* The kind of object (pg_shdepend's classid) a maker record of tag is of */
static Oid
maker_class(char tag)
{
	return tag == TAG_CREATOR ? RefsBranchClassId : RefsTagClassId;
}

/*
 * Make role (InvalidOid: none) the recorded maker of an object of kind
 * classId, *rec its record (rec->role InvalidOid: none yet), and its role's
 * shared dependency so: a new one has an id given, one given up its
 * dependency dropped, and one whose maker changes keeps its id.
 */
static void
maker_change(Oid classId, MakerRecord *rec, Oid role)
{
	if (rec->role == role)
		return;
	if (!OidIsValid(role))
	{
		deleteSharedDependencyRecordsFor(classId, rec->id, 0);
		rec->id = InvalidOid;
	}
	else if (!OidIsValid(rec->role))
	{
		rec->id = newMadeObjectId(classId);
		recordDependencyOnOwner(classId, rec->id, role);
	}
	else
		changeDependencyOnOwner(classId, rec->id, role);
	rec->role = role;
}

/* Branch (tag) name's maker record; false (*rec none) if none is recorded. */
static bool
maker_record(char tag, const char *name, MakerRecord *rec)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;

	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, tag);
	appendStringInfoString(&key, name);
	found = refs_get(key.data, key.len, &v) && v.len == MAKER_RECORD_LEN;
	if (found)
	{
		memcpy(&rec->role, v.data, sizeof(Oid));
		memcpy(&rec->id, v.data + sizeof(Oid), sizeof(Oid));
	}
	else
		rec->role = rec->id = InvalidOid;
	pfree(key.data);
	pfree(v.data);
	return found;
}

/*
 * Record role (InvalidOid: none) as the role that made branch (tag) name,
 * with its shared dependency (maker_change()).
 */
static void
maker_put(char tag, const char *name, Oid role)
{
	StringInfoData key;
	StringInfoData v;
	MakerRecord rec;

	(void) maker_record(tag, name, &rec);
	if (rec.role == role)
		return;
	maker_change(maker_class(tag), &rec, role);
	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, tag);
	appendStringInfoString(&key, name);
	appendBinaryStringInfo(&v, &rec.role, sizeof(Oid));
	appendBinaryStringInfo(&v, &rec.id, sizeof(Oid));
	refs_put(key.data, key.len, OidIsValid(role) ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
}

/* The role that made branch (tag) name; false if none is recorded. */
static bool
maker_get(char tag, const char *name, Oid *role)
{
	MakerRecord rec;

	if (!maker_record(tag, name, &rec))
		return false;
	*role = rec.role;
	return true;
}

static bool copies_commit_get(const ProllyHash *id, ProllyCommitInfo *info);
static bool branch_baseline_root(const char *name, ProllyNodeRef *root);

/*
 * The commit this backend wrote last, and its root: a head moved to it
 * (a commit's) is indexed by its root without reading it back.  An id
 * names its content, so what it holds never changes.
 */
static ProllyHash last_written_commit;
static ProllyHash last_written_root;
static ProllyHash prior_written_commit;	/* and the one before it */
static ProllyHash prior_written_root;

/* The root of commit, if it is one this backend wrote last or before. */
static bool
written_root(const ProllyHash *commit, ProllyHash *root)
{
	if (commit->len > 0 && same_hash(commit, &last_written_commit))
		*root = last_written_root;
	else if (commit->len > 0 && same_hash(commit, &prior_written_commit))
		*root = prior_written_root;
	else
		return false;
	return true;
}

/* The root of commit head (false: no such commit). */
static bool
head_commit_root(const ProllyHash *head, ProllyHash *root)
{
	ProllyCommitInfo info;

	if (written_root(head, root))
		return true;
	if (!copies_commit_get(head, &info))
		return false;
	*root = info.root.hash;
	return true;
}

/* An empty value, for an index's entries. */
static const StringInfoData index_value = {"", 0, 1, 0};

/* A root's index key: the root, the kind ('h' head, 'w' baseline), name. */
static void
root_of_key(StringInfo key, const ProllyHash *root, char kind, const char *name)
{
	appendStringInfoChar(key, TAG_ROOT_OF);
	put_hash(key, root);
	appendStringInfoChar(key, kind);
	appendStringInfoString(key, name);
}

/* The case-folded name index's key of branch name. */
static void
folded_key(StringInfo key, const char *name)
{
	appendStringInfoChar(key, TAG_FOLDED);
	for (const char *c = name; *c; c++)
		appendStringInfoChar(key, pg_ascii_tolower((unsigned char) *c));
	appendStringInfoChar(key, '\0');
	appendStringInfoString(key, name);
}

/* Put (value NULL: delete) index entry key, as one of its branch's. */
static void
index_put(const StringInfoData *key, bool present)
{
	refs_put(key->data, key->len, present ? &index_value : NULL);
}

/* The root of the head a 'B' value names, if it records one. */
static bool
head_value_root(const StringInfoData *v, ProllyHash *root)
{
	ProllyHash	head;
	const char *p = get_hash(v->data, v->data + v->len, &head);

	if (p >= v->data + v->len)
		return false;
	(void) get_hash(p, v->data + v->len, root);
	return root->len > 0;
}

/* The root branch name's head is at, as its 'B' entry records it. */
static bool
head_root_get(const char *name, ProllyHash *root)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;

	initStringInfo(&key);
	initStringInfo(&v);
	branch_key(name, &key);
	found = refs_get(key.data, key.len, &v) && head_value_root(&v, root);
	pfree(key.data);
	pfree(v.data);
	return found;
}

/*
 * Fold branch name's name into the case-folded index (present) or take it
 * out: as the branch is made (with a head or not), renamed and deleted.
 */
static void
folded_index_set(const char *name, bool present)
{
	StringInfoData key;

	initStringInfo(&key);
	folded_key(&key, name);
	index_put(&key, present);
	pfree(key.data);
}

/* The same, put only if it isn't there. */
static void
folded_index_note(const char *name)
{
	StringInfoData key;
	StringInfoData v;

	initStringInfo(&key);
	initStringInfo(&v);
	folded_key(&key, name);
	if (!refs_get(key.data, key.len, &v))
		index_put(&key, true);
	pfree(key.data);
	pfree(v.data);
}

/*
 * Index branch name's head by its root: was the root its head had (NULL:
 * none recorded), root the one it has now (NULL: none), the entries
 * changed only if they change.
 */
static void
head_index_set(const char *name, const ProllyHash *was, const ProllyHash *root)
{
	StringInfoData key;

	if (was != NULL && root != NULL && same_hash(was, root))
		return;
	initStringInfo(&key);
	if (was != NULL)
	{
		root_of_key(&key, was, 'h', name);
		index_put(&key, false);
	}
	if (root != NULL)
	{
		resetStringInfo(&key);
		root_of_key(&key, root, 'h', name);
		index_put(&key, true);
	}
	pfree(key.data);
}

/* Index branch name's baseline's fold's root (NULL: none), was was. */
static void
baseline_index_set(const char *name, const ProllyNodeRef *was,
				   const ProllyNodeRef *root)
{
	StringInfoData key;

	if (was != NULL && root != NULL && same_hash(&was->hash, &root->hash))
		return;
	initStringInfo(&key);
	if (was != NULL)
	{
		root_of_key(&key, &was->hash, 'w', name);
		index_put(&key, false);
	}
	if (root != NULL)
	{
		resetStringInfo(&key);
		root_of_key(&key, &root->hash, 'w', name);
		index_put(&key, true);
	}
	pfree(key.data);
}

/*
 * Make head (NULL: none) branch name's head commit, noting the transaction
 * that moved it (head_moved_unseen()).  A branch that gets its first head
 * here (a fetch's, a clone's or a push's new branch) is the session role's,
 * unless it was made with a role recorded already; main is never anyone's.
 */
static void
branch_set_head(const char *name, const ProllyHash *head)
{
	StringInfoData key;
	StringInfoData v;
	FullTransactionId xid = head ? GetTopFullTransactionId() :
		InvalidFullTransactionId;
	Oid			role;
	ProllyHash	was;
	ProllyHash	was_head;
	ProllyHash	root;
	bool		found;
	bool		had_root;
	bool		have_root;

	initStringInfo(&key);
	initStringInfo(&v);
	branch_key(name, &key);
	/* (the entry it had: its head's root, for the index of roots) */
	found = refs_get(key.data, key.len, &v);
	had_root = found && head_value_root(&v, &was);
	memset(&was_head, 0, sizeof(was_head));
	if (found)
		(void) get_hash(v.data, v.data + v.len, &was_head);
	if (head && strcmp(name, "main") != 0 && !found &&
		!maker_get(TAG_CREATOR, name, &role))
		maker_put(TAG_CREATOR, name, GetUserId());
	have_root = head != NULL && head_commit_root(head, &root);
	resetStringInfo(&v);
	if (head)
		put_hash(&v, head);
	if (have_root)
		put_hash(&v, &root);
	refs_put(key.data, key.len, head ? &v : NULL);
	head_index_set(name, had_root ? &was : NULL, have_root ? &root : NULL);

	/*
	 * (a branch first given a head here, by a fetch, a clone or a push,
	 * may have been made by none of the paths that fold its name)
	 */
	if (head != NULL && !found)
		folded_index_note(name);

	resetStringInfo(&key);
	resetStringInfo(&v);
	appendStringInfoChar(&key, TAG_MOVER);
	appendStringInfoString(&key, name);
	appendBinaryStringInfo(&v, &xid, sizeof(xid));
	if (head)
		head_log_note(name, &was_head, head, &v);
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
		if (v.len >= sizeof(FullTransactionId))
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

/*
 * The baseline of a branch whose working set is being folded only to be
 * compared (settle_compared()): its trees are the backend's
 * (prolly_local_chunks_begin()'s), so it is kept here, not in the refs,
 * whose chunks are the store's, until its chunks are persisted.
 */
static struct
{
	bool		active;
	char	   *name;
	bool		have;
	ProllyNodeRef baseline;
	bool		have_root;
	ProllyNodeRef root;
}			compared;

static bool
compared_branch(const char *name)
{
	return compared.active && strcmp(compared.name, name) == 0;
}

/* A branch's baseline; false if it has none. */
static bool
branch_baseline(const char *name, ProllyNodeRef *baseline)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;
	ProllyHash	h;

	if (compared_branch(name))
	{
		if (compared.have)
			*baseline = compared.baseline;
		return compared.have;
	}
	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_BASELINE);
	appendStringInfoString(&key, name);
	found = refs_get(key.data, key.len, &v);
	baseline_seen_value(name, &v, found);
	found = found && get_hash(v.data, v.data + v.len, &h) && h.len > 0;
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

	if (compared_branch(name))
	{
		if (compared.have && compared.have_root)
			*root = compared.root;
		return compared.have && compared.have_root;
	}
	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_BASELINE);
	appendStringInfoString(&key, name);
	if (refs_get(key.data, key.len, &v))
	{
		const char *p = get_hash(v.data, v.data + v.len, &h);

		baseline_seen_value(name, &v, true);
		if (p < v.data + v.len)
		{
			get_hash(p, v.data + v.len, &h);
			found = h.len > 0;
			if (found)
				ref_of(&h, root);
		}
	}
	else
		baseline_seen_value(name, &v, false);
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
	FullTransactionId xid;
	ProllyNodeRef was;
	bool		had;

	if (compared_branch(name))
	{
		compared.have = baseline != NULL;
		if (baseline)
			compared.baseline = *baseline;
		compared.have_root = baseline != NULL && root != NULL;
		if (compared.have_root)
			compared.root = *root;
		return;
	}
	xid = baseline ? GetTopFullTransactionId() : InvalidFullTransactionId;
	/* (the fold root it had, as last read or written in the transaction) */
	if (baseline_seen_name != NULL && strcmp(baseline_seen_name, name) == 0)
	{
		had = baseline_seen_have;
		was = baseline_seen_root;
	}
	else
		had = branch_baseline_root(name, &was);
	baseline_index_set(name, had ? &was : NULL, baseline != NULL ? root : NULL);
	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_BASELINE);
	appendStringInfoString(&key, name);
	if (baseline)
		put_hash(&v, &baseline->hash);
	if (baseline && root)
		put_hash(&v, &root->hash);
	refs_put(key.data, key.len, baseline ? &v : NULL);
	/* (what it is now, noted once the branch is held: refs_put() holds it) */
	if (branch_held(name, strlen(name)))
		baseline_seen_set(name, baseline != NULL && root != NULL,
						  baseline != NULL && root != NULL ? &root->hash : NULL);

	resetStringInfo(&key);
	resetStringInfo(&v);
	appendStringInfoChar(&key, TAG_BASELINE_MOVER);
	appendStringInfoString(&key, name);
	appendBinaryStringInfo(&v, &xid, sizeof(xid));
	refs_put(key.data, key.len, baseline ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
}

/*
 * The id of the branch called name; InvalidOid if there's none (its
 * pg_branch row found by its name, as the session sees the catalog).
 */
static Oid
branch_oid(const char *name)
{
	HeapTuple	tup;
	Oid			oid;

	if (strlen(name) >= NAMEDATALEN)
		return InvalidOid;
	tup = SearchSysCache1(BRANCHNAME, CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		return InvalidOid;
	oid = ((Form_pg_branch) GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);
	return oid;
}

/*
 * The name of the branch whose id is branch (palloc'd); NULL if there's
 * none.  Its pg_branch row found by its id, so the work doesn't grow with
 * the number of branches.
 */
static char *
branch_name_of(Oid branch)
{
	HeapTuple	tup;
	char	   *name;

	tup = SearchSysCache1(BRANCHOID, ObjectIdGetDatum(branch));
	if (!HeapTupleIsValid(tup))
		return NULL;
	name = pstrdup(NameStr(((Form_pg_branch) GETSTRUCT(tup))->brname));
	ReleaseSysCache(tup);
	return name;
}

/* A branch with copies, by name and id. */
typedef struct NamedBranch
{
	char	   *name;
	Oid			branch;
} NamedBranch;

static int
named_branch_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(((const NamedBranch *) lfirst(a))->name,
				  ((const NamedBranch *) lfirst(b))->name);
}

/*
 * The branches with copies a fold of is known (the generation table's,
 * of a fixed size), in name order, as a listing of every branch would
 * visit them: the work is in those, however many branches there are.
 */
static List *
known_fold_branches(void)
{
	Oid		   *oids = palloc_array(Oid, BRANCH_FOLD_KNOWN_MAX);
	int			n = branch_fold_known_branches(oids, BRANCH_FOLD_KNOWN_MAX);
	List	   *out = NIL;

	for (int i = 0; i < n; i++)
	{
		char	   *name = branch_name_of(oids[i]);
		NamedBranch *nb;

		if (name == NULL)
			continue;
		nb = palloc(sizeof(NamedBranch));
		nb->name = name;
		nb->branch = oids[i];
		out = lappend(out, nb);
	}
	pfree(oids);
	list_sort(out, named_branch_cmp);
	return out;
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
 * Either way, the working set's identity anchor is then the commit's: its
 * objects are named as the commit names them.
 */
static void anchor_at_commit(const ProllyCommitInfo *commit);
static void settle_rows(const ProllyNodeRef *target, const KnownSource *source);

static void restore_pages(const ProllyCommitInfo *commit);
static void keep_pages(const ProllyHash *commit_id, const ProllyHash *parent,
					   const ProllyNodeRef *root,
					   Snapshot snap);

/*
 * The move under way's waits for holders of its relations' locks (branch
 * operations' share(), share_kept() and restore(), for a lock not held
 * already): one set of bounds covers the waits of every table it restores
 * or shares.
 */
static BranchLockWait move_wait;

static void
settle(const ProllyCommitInfo *commit, const KnownSource *source)
{
	memset(&move_wait, 0, sizeof(move_wait));
	if (source == NULL)
		restore_pages(commit);
	settle_rows(&commit->root, source);
	anchor_at_commit(commit);
	if (ActiveSnapshotSet())
		keep_pages(&commit->id, commit->nparents > 0 ? &commit->parents[0] : NULL,
				   &commit->root, GetActiveSnapshot());
}

static bool share_clean_tables(const ProllyNodeRef *target,
							   ProllyNodeRef *working);
static bool advance_to_target(const ProllyNodeRef *target,
							  const ProllyNodeRef *working);

/*
 * The end of a fold only compared: the branch's baseline as the step left
 * it, its chunks the backend kept persisted (only those it reaches: a
 * baseline advanced to the target names the target's trees, stored
 * already), made the branch's in the refs; the working set's root with it
 * only if the store has it (the target's).  The rest of what the backend
 * kept is forgotten.
 */
static void
compared_end(bool keep)
{
	char	   *name = compared.name;
	bool		have = compared.have;
	ProllyNodeRef baseline = compared.baseline;
	bool		have_root = compared.have_root;
	ProllyNodeRef root = compared.root;

	if (keep && have)
		prolly_local_chunks_persist(&baseline.hash);
	compared.active = false;
	prolly_local_chunks_end();
	if (keep)
	{
		refs_init();
		if (have_root &&
			!PROLLY_CALL(env.chunk_store, has, env.chunks, &root.hash))
			have_root = false;
		branch_set_baseline(name, have ? &baseline : NULL,
							have_root ? &root : NULL);
	}
	pfree(name);
}

/*
 * settle_rows() of the session's branch (name, its baseline as given),
 * whose working set has to be folded to be compared with the target: the
 * fold, and every tree the move builds on it, kept in the backend's memory
 * (prolly_local_chunks_begin()), not put in the store.  None of them is
 * kept once the move is over but the baseline as it then is, which
 * advance_to_target() has made the target's but for its finders' state:
 * so only its own few nodes are put.  (Where it couldn't be advanced, the
 * fold's trees it still names are put too, as they would have been; and
 * the branch is folded again from it, as settle_rows() does.)  False,
 * nothing done, where the fold caches can't be used.
 */
static bool
settle_compared(const char *name, const ProllyNodeRef *target,
				const ProllyNodeRef *baseline)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef working;
	ProllyNodeRef next;
	bool		advanced = true;

	if (!fold_caches_usable() || prolly_local_chunks_active() ||
		RecoveryInProgress())
		return false;
	prolly_database_components(&dc);
	compared.name = MemoryContextStrdup(TopTransactionContext, name);
	compared.have = baseline != NULL;
	if (baseline)
		compared.baseline = *baseline;
	compared.have_root = false;
	compared.active = true;
	prolly_local_chunks_begin();
	PG_TRY();
	{
		fold_from_baseline(&working, &next, false);
		branch_set_baseline(name, &next, &working);
		if (!same_hash(&working.hash, &target->hash) &&
			!(share_clean_tables(target, &working) &&
			  same_hash(&working.hash, &target->hash)))
		{
			PROLLY_CALL(dc.diff_merge, move_to, target, &working);
			advanced = advance_to_target(target, &working);
		}
		compared_end(true);
	}
	PG_CATCH();
	{
		if (compared.active)
			compared_end(false);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (!advanced)
		fold_as_baseline(NULL);
	return true;
}

static void
settle_rows(const ProllyNodeRef *target, const KnownSource *source)
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

			if (settle_compared(name, target, have ? &baseline : NULL))
				return;
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
 * Sharing pages instead of writing rows (#491).  A move whose target has a
 * table whose rows differ from the working set's gives the table the
 * storage of another state that holds the target's (the table's with its
 * indexes', TOAST table's and change log's), when sharing costs less than
 * writing the rows that differ: its pages, maps and log then are that
 * state's, and the baseline takes that state's entry for it, which its log
 * goes on from.  A source is a branch other than the session's whose
 * working set is a root known: one known as folded (nobody writing it,
 * every writer that ended seen), or a kept materialization (the
 * materialization cache's: immutable, made before the snapshot), whose
 * root has the target's table, the same data tree and definition.  Only a
 * move of rows (the two roots define the same objects), only of tables
 * whose relations are the source's too (made from the same, so the same
 * ids) and stored alike, and only where the locks are free now (share()'s:
 * this branch's copies as TRUNCATE locks them, the source's against its
 * writers and its deletion), a known source still known at its root as it
 * was known once they are held (the first table shared changes pg_class,
 * after which nothing new is known in the transaction).  At a first use
 * only kept sources serve: the copies' catalog rows, made in the
 * transaction, keep anything from being known after them.  As TRUNCATE,
 * it isn't MVCC-safe: a snapshot taken before the move commits reads the
 * shared storage after.
 *
 * Sharing a block-mapped relation copies its map's root page, each subtree
 * gaining a holder, and its free space and visibility maps: a constant for
 * each relation, and a little for each block those maps cover, against
 * about 11 us for each row written.
 */
#define MOVE_ROW_USEC		11
#define SHARE_RELATION_USEC	300
#define SHARE_BLOCKS_PER_USEC	64

/* A root's table, as tables() visits it. */
typedef struct NamedTree
{
	char	   *name;
	ProllyNodeRef data;
	bool		readable;
	char	   *description;	/* its description's bytes, and their length */
	uint32		description_len;
} NamedTree;

static void
note_table(const char *name, const ProllyNodeRef *data,
		   const ProllyValue *description, const char *unreadable, void *arg)
{
	NamedTree  *t = palloc0(sizeof(NamedTree));

	t->name = pstrdup(name);
	t->data = *data;
	t->readable = unreadable == NULL;
	if (description != NULL)
	{
		t->description = palloc(Max(description->len, 1));
		memcpy(t->description, description->data, description->len);
		t->description_len = description->len;
	}
	*(List **) arg = lappend(*(List **) arg, t);
}

/* Are two roots' tables the same: name, rows and definition? */
static bool
same_table(const NamedTree *a, const NamedTree *b)
{
	return a->readable && b->readable && strcmp(a->name, b->name) == 0 &&
		same_hash(&a->data.hash, &b->data.hash) &&
		a->description_len == b->description_len &&
		memcmp(a->description, b->description, a->description_len) == 0;
}

/* A count of a diff's changes, which stops the diff at max of them. */
typedef struct ChangeCount
{
	uint64		n;
	uint64		max;
} ChangeCount;

static bool
count_change(ProllyDiffKind kind, const ProllyKey *key,
			 const ProllyValue *before, const ProllyValue *after, void *arg)
{
	ChangeCount *c = arg;

	return ++c->n < c->max;
}

/*
 * How many keys from and to differ by, but no more than max: the diff
 * stops at the max'th change, having read only the nodes up to it, so a
 * caller that compares the count with a bound reads O((bound + 1) log n)
 * nodes however far apart the trees are.
 */
static uint64
count_diff_upto(const ProllyNodeRef *from, const ProllyNodeRef *to,
				uint64 max)
{
	ProllyTreeEnv denv;
	ChangeCount c = {0, max};

	if (max == 0 || same_hash(&from->hash, &to->hash))
		return 0;
	prolly_folded_tree_env(&denv);
	PROLLY_CALL(tree, diff, &denv, from, to, count_change, &c);
	return c.n;
}

/* The fewest rows whose writing (MOVE_ROW_USEC each) costs more than usec. */
static uint64
rows_costing_more(double usec)
{
	uint64		n;

	if (!(usec < (double) PG_INT64_MAX / MOVE_ROW_USEC))
		return PG_UINT64_MAX;
	n = usec > 0 ? (uint64) (usec / MOVE_ROW_USEC) : 0;
	while (n > 0 && (double) (n - 1) * MOVE_ROW_USEC > usec)
		n--;
	while ((double) n * MOVE_ROW_USEC <= usec)
		n++;
	return n;
}

/*
 * Prolly table relid and the relations stored with it: its indexes, its
 * TOAST table and change log, and theirs; their blocks counted (if
 * blocks isn't NULL).
 */
static List *
stored_with(Oid relid, uint64 *blocks)
{
	List	   *relids = list_make1_oid(relid);
	ProllyComponents c;

	if (blocks)
		*blocks = 0;
	for (int i = 0; i < list_length(relids); i++)
	{
		Relation	rel = relation_open(list_nth_oid(relids, i), AccessShareLock);

		if (blocks)
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

/* A state another branch holds, whose tables a move can share. */
typedef struct ShareSource
{
	char	   *name;
	Oid			branch;
	ProllyNodeRef root;			/* its working set */
	ProllyNodeRef baseline;
	bool		kept;			/* a kept materialization; else known: */
	BranchFoldStamp at;			/* what it was known at */
	const ProllyKeptCommit *kept_commit;	/* a kept commit's pages: its
											 * tables given storage made of
											 * them (share_kept()) */
	List	   *tables;			/* NamedTree *: root's, once read */
	bool		read;
	List	   *shared;			/* names of the tables shared from it */
} ShareSource;

typedef struct ShareSources
{
	Snapshot	snap;
	List	   *sources;		/* ShareSource * */
} ShareSources;

static void
find_known_source(const char *name, Oid branch, void *arg)
{
	ShareSources *ss = arg;
	ShareSource *s;
	ProllyNodeRef baseline;
	ProllyHash	h;
	BranchFoldStamp at;

	if (branch == MyBranch || !branch_baseline(name, &baseline) ||
		!branch_fold_known_at(branch, ss->snap, &baseline.hash, false, &h,
							  &at))
		return;
	s = palloc0(sizeof(ShareSource));
	s->name = pstrdup(name);
	s->branch = branch;
	ref_of(&h, &s->root);
	s->baseline = baseline;
	s->at = at;
	ss->sources = lappend(ss->sources, s);
}

static void
find_kept_source(const ProllyKeptState *state, void *arg)
{
	ShareSources *ss = arg;
	ShareSource *s;
	ProllyNodeRef baseline;

	if (state->kind == PROLLY_KEPT_COMMIT)
	{
		/* (its baseline then, which the tables' change logs go on from) */
		if (state->kept->baseline.len == 0)
			return;
		foreach_ptr(ShareSource, known, ss->sources)
			if (known->kept_commit && known->branch == state->branch &&
				same_hash(&known->kept_commit->commit, &state->commit))
				return;
		ref_of(&state->kept->baseline, &baseline);
	}
	else
	{
		foreach_ptr(ShareSource, known, ss->sources)
			if (known->branch == state->branch && known->kept_commit == NULL)
				return;
		if (!branch_baseline(state->name, &baseline))
			return;
	}
	s = palloc0(sizeof(ShareSource));
	s->name = pstrdup(state->name);
	s->branch = state->branch;
	s->root = state->root;
	s->baseline = baseline;
	s->kept = true;
	s->kept_commit = state->kept;
	ss->sources = lappend(ss->sources, s);
}

/*
 * The kept states that could give one of tables (NamedTree *, a target's)
 * its pages: those holding its data tree (the materialization cache's
 * filter of them).
 */
static void
kept_filter_of_tables(List *tables, ProllyKeptFilter *filter)
{
	ProllyHash *hashes = palloc_array(ProllyHash, Max(list_length(tables), 1));
	int			n = 0;

	foreach_ptr(NamedTree, t, tables)
		if (t->readable)
			hashes[n++] = t->data.hash;
	filter->commit = NULL;
	filter->tables = hashes;
	filter->ntables = n;
}

/* A kept commit's table called name, or NULL. */
static const ProllyKeptTable *
kept_table(const ProllyKeptCommit *kc, const char *name)
{
	for (int t = 0; t < kc->ntables; t++)
	{
		if (strcmp(kc->tables[t].name, name) == 0)
			return &kc->tables[t];
	}
	return NULL;
}

/* Are relids just the relations kept (nrels of them, in any order)? */
static bool
same_relation_set(List *relids, const BranchKeptRelation *rels, int nrels)
{
	if (list_length(relids) != nrels)
		return false;
	for (int r = 0; r < nrels; r++)
	{
		if (!list_member_oid(relids, rels[r].relid))
			return false;
	}
	return true;
}

static bool may_restore(Oid relid);
static List *kept_relations(Oid relid);
static Oid	table_named(const char *name);

/*
 * Does every snapshot that could read the session's branch see the
 * transaction that kept a commit's pages committed?  Each of the pages'
 * tuples was written, and deleted if it is, by a transaction that ended
 * before that one's snapshot, so such a snapshot sees exactly the commit's
 * rows in them.  One taken before (on any branch: a session may check out
 * this one while it holds it) could see versions another branch made and
 * deleted, rows that were never this branch's nor any commit's.  Asked of
 * every backend's oldest snapshot in the cluster (the shared catalogs'
 * horizon), the mover's own among them.
 */
static bool
kept_before_every_snapshot_xid(FullTransactionId kept_by)
{
	TransactionId xid = XidFromFullTransactionId(kept_by);

	return TransactionIdIsNormal(xid) &&
		TransactionIdPrecedes(xid, GetOldestNonRemovableTransactionId(NULL));
}

static bool
kept_before_every_snapshot(const ProllyKeptCommit *kc)
{
	return kept_before_every_snapshot_xid(kc->kept_by);
}

/*
 * Can a kept commit give table name (the target's, as the commit has it)
 * its pages: its kept relations are the table's now (the table's own
 * first), the user could write its rows (as restore_pages() asks), and
 * every snapshot sees its pages as the commit's rows?
 */
static bool
kept_commit_serves(const ProllyKeptCommit *kc, const char *name)
{
	const ProllyKeptTable *kt = kept_table(kc, name);
	Oid			relid;
	List	   *relids;

	if (branch_ops()->share_kept == NULL || kt == NULL ||
		!kept_before_every_snapshot(kc) ||
		!OidIsValid(relid = table_named(name)) || kt->rels[0].relid != relid)
		return false;
	relids = kept_relations(relid);
	return relids != NIL && same_relation_set(relids, kt->rels, kt->nrels) &&
		may_restore(relid);
}

/*
 * Is source still at its root: a known one as it was known (asked of what
 * was known then: the first table shared changes pg_class, after which
 * nothing new is known in the transaction, though nothing about the other
 * branch changed), a kept one always (share() holds it against deletion).
 */
static bool
still_at_root(void *arg)
{
	ShareSource *s = arg;

	return s->kept || branch_fold_unchanged(&s->at);
}

/*
 * The first source with table t as the target has it that can give it its
 * pages, or NULL: a kept commit only where it serves (kept_commit_serves()),
 * so that a share a source would refuse leaves the later ones to try, and
 * the planner costs only shares the move can make.
 */
static ShareSource *
source_of(List *sources, const NamedTree *t)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	foreach_ptr(ShareSource, s, sources)
	{
		if (!s->read)
		{
			PROLLY_CALL(dc.canonicalizer, tables, &s->root, NULL, note_table,
						&s->tables);
			s->read = true;
		}
		foreach_ptr(NamedTree, theirs, s->tables)
		{
			if (!same_table(theirs, t))
				continue;
			if (s->kept_commit == NULL ||
				kept_commit_serves(s->kept_commit, t->name))
				return s;
			break;
		}
	}
	return NULL;
}

struct KeptCommit;
static List *kept_get(const char *name);
static void kept_put(const char *name, List *kept);
static List *kept_index_keys(const char *name, List *kept);

static int	kept_carried(List *kept, const struct KeptCommit *was,
						 const char *name, BranchKeptRelation **carry,
						 BranchKeptRelation ***into);

/*
 * The tables of the session's branch whose rows differ from target's made
 * the copies another state has where that costs less (true: some were);
 * *working is then the branch's working set, folded as its baseline.
 */
static bool
share_clean_tables(const ProllyNodeRef *target, ProllyNodeRef *working)
{
	ProllyDatabaseComponents dc;
	ShareSources ss = {0};
	List	   *ours = NIL;
	List	   *theirs = NIL;
	List	   *chosen = NIL;	/* NamedTree *, then each one's relations */
	List	   *relations = NIL;
	List	   *from = NIL;		/* and its source */
	ProllyNodeRef baseline;
	ProllyNodeRef next;
	ListCell   *a;
	ListCell   *b;
	ListCell   *c;
	char	   *current = branch_ops()->current();
	int			nshared = 0;
	List	   *kept = NIL;		/* the branch's kept commits, if shared from */
	bool		renamed = false;

	prolly_database_components(&dc);
	if (branch_ops()->share == NULL || dc.canonicalizer->splice_baseline == NULL ||
		!fold_caches_usable() || !branch_baseline(current, &baseline))
		return false;
	ss.snap = fold_snapshot();
	foreach_ptr(NamedBranch, nb, known_fold_branches())
		find_known_source(nb->name, nb->branch, &ss);
	PROLLY_CALL(dc.canonicalizer, tables, target, NULL, note_table, &theirs);
	if (dc.matcache && PROLLY_HAS(dc.matcache, visit))
	{
		ProllyKeptFilter filter;

		kept_filter_of_tables(theirs, &filter);
		PROLLY_CALL(dc.matcache, visit, ss.snap, &filter, find_kept_source, &ss);
	}
	if (ss.sources == NIL ||
		!PROLLY_CALL(dc.canonicalizer, same_schema, working, target))
		return false;

	/* the tables whose rows differ, where a source has them and sharing costs less */
	PROLLY_CALL(dc.canonicalizer, tables, working, NULL, note_table, &ours);
	if (list_length(ours) != list_length(theirs))
		return false;
	forboth(a, ours, b, theirs)
	{
		NamedTree  *o = lfirst(a);
		NamedTree  *t = lfirst(b);
		ShareSource *s;
		uint64		changes;
		uint64		blocks;
		double		share_usec;
		Oid			relid;
		List	   *relids;

		if (strcmp(o->name, t->name) != 0)
			return false;
		if (same_hash(&o->data.hash, &t->data.hash) || !o->readable ||
			!t->readable)
			continue;
		relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(o->name, NULL)),
								 NoLock, true);
		if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION ||
			(s = source_of(ss.sources, t)) == NULL)
			continue;
		relids = stored_with(relid, &blocks);
		share_usec = (double) list_length(relids) * SHARE_RELATION_USEC +
			(double) blocks / SHARE_BLOCKS_PER_USEC;
		/* (counted only as far as the rows that cost more than sharing) */
		changes = count_diff_upto(&o->data, &t->data,
								  rows_costing_more(share_usec));
		if ((double) changes * MOVE_ROW_USEC <= share_usec)
			continue;
		chosen = lappend(chosen, o);
		relations = lappend(relations, relids);
		from = lappend(from, s);
	}
	if (chosen == NIL)
		return false;
	if (debug_prolly_verify_fold_caches)
		foreach_ptr(ShareSource, s, ss.sources)
			if (list_member_ptr(from, s) && s->kept_commit == NULL)
				verify_known(s->name, false, &s->root);

	/*
	 * (each shared if nobody uses it now, and its source is still at its
	 * root with it locked: none of its writers ended since, nor can write it
	 * now)
	 */
	forthree(a, chosen, b, relations, c, from)
	{
		ShareSource *s = lfirst(c);
		const char *table = ((NamedTree *) lfirst(a))->name;
		bool		done;

		if (s->kept_commit != NULL)
		{
			const ProllyKeptTable *kt = kept_table(s->kept_commit, table);
			BranchKeptRelation *carry;
			BranchKeptRelation *carried;
			BranchKeptRelation **into;
			int			ncarry = 0;

			/*
			 * (the session's own kept pages: the branch's kept commits'
			 * snapshots of the table carried into its new storage, as a
			 * restore carries them, and their entries renamed)
			 */
			if (s->branch == MyBranch && kept == NIL)
				kept = kept_get(current);
			if (s->branch == MyBranch)
				ncarry = kept_carried(kept, NULL, table, &carry, &into);
			else
				carry = NULL;
			carried = palloc_array(BranchKeptRelation, Max(ncarry, 1));
			done = branch_ops()->share_kept(s->branch, kt->rels, kt->nrels,
											carry, ncarry, carried, NULL,
											NULL, &move_wait);
			for (int j = 0; done && j < ncarry; j++)
				*into[j] = carried[j];
			renamed |= done && ncarry > 0;
		}
		else
			done = branch_ops()->share(s->branch, lfirst(b), still_at_root, s,
									   &move_wait);
		if (done)
		{
			s->shared = lappend(s->shared, ((NamedTree *) lfirst(a))->name);
			nshared++;
		}
	}
	if (renamed)
		kept_put(current, kept);
	if (nshared == 0)
		return false;
	pgstat_count_prolly(PGSTAT_PROLLY_MOVE_TABLES_SHARED, nshared);
	foreach_ptr(ShareSource, s, ss.sources)
	{
		ProllyNodeRef spliced;

		if (s->shared == NIL)
			continue;
		PROLLY_CALL(dc.canonicalizer, splice_baseline, &baseline, &s->baseline,
					s->shared, &spliced);
		baseline = spliced;
	}
	branch_set_baseline(current, &baseline, NULL);
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

/* A branch's copies moved to a commit (copies_branch_move()). */
typedef struct MoveSettled
{
	ProllyCommitInfo *info;
	ProllyHash	head;			/* the commit moved to */
} MoveSettled;

/*
 * The branch's copies moved to the commit (move_settled()), the tables it
 * makes the relations the commit was made from; refused, the error undoing
 * it, if it put into a table a version of another relation
 * (prolly_refuse_mismatched_versions()), as a hard reset is, unless the
 * branch is replaced (prolly_move_replaces): its tables that are other
 * relations are dropped first, and the move makes the head's.
 */
bool		prolly_move_replaces = false;

static void
move_settled_checked(void *arg)
{
	MoveSettled *ms = arg;
	ProllyNodeRef before;
	List	   *relids = NIL;
	bool		folded;

	/* (replaced, its tables other relations than the head's: dropped) */
	if (prolly_move_replaces && prolly_versions_may_mismatch(&ms->head, 1))
		prolly_drop_mismatched_tables(&ms->head, "move the branch", false);
	folded = prolly_versions_may_mismatch(&ms->head, 1) &&
		copies_working_set(NULL, &before);
	if (folded)
		relids = prolly_relations_of(&before);
	prolly_move_provenance = list_make1(&ms->head);
	PG_TRY();
	{
		move_settled(ms->info);
	}
	PG_FINALLY();
	{
		prolly_move_provenance = NIL;
	}
	PG_END_TRY();
	if (folded)
		prolly_refuse_mismatched_versions(&before, relids, &ms->head, 1,
										  PROLLY_MISMATCH_PUSH);
}

/*
 * Kept pages (#767).  When a branch's working set is known to be a commit's
 * root (a commit's fold made it; a move settled there), the pages of its
 * tables are kept as they are: each block-mapped relation stored with a
 * table (its heap, indexes, TOAST table and change log) by a root of its
 * storage captured into its page store (branch operations' keep()), O(1)
 * in its size, and only those that changed since a commit kept already
 * take a new one.  The
 * refs keep, for each branch, the last prolly.kept_commits commits so kept
 * (TAG_KEPT): each with the branch's baseline then, and each table's name
 * and relations.  A move back to one of them (a hard reset to the head, or
 * to one before) gives each table the pages kept (restore()): new storage
 * made of each root, O(1), for each relation that changed since, and
 * the baseline takes the table's entry from the kept one (its change log
 * is as it was then too), so its fold reads nothing.  Whatever else differs
 * (definitions, sequences, a table that couldn't be restored) the move then
 * writes as any move does.
 *
 * Pages are kept only while no other transaction can be writing them: the
 * branch's relations are locked against writers, without waiting, and the
 * snapshot the commit or move went by must have seen every writer of the
 * branch that ended (branch_writers_settled()).  So the pages hold exactly
 * the commit's rows, and the keeping transaction's own, which commit with
 * it.  At a transaction's own commit (dolt.transaction_commit), whose head
 * moves at every transaction, only once a root is due (root_due()).
 *
 * A branch's first use at a commit another branch keeps the pages of is
 * made of them (make_of_kept(), branch operations' make_kept(), #489):
 * each table's relations share the snapshots' pages, O(1), and the
 * source's writers are never locked out for them, the pages being as
 * they were at the commit whatever is written since.
 *
 * So a commit no longer kept (older than the last prolly.kept_commits) may
 * still be being read by another's first use, which read it from the refs
 * before.  It is retired, not let go of: its entry stays, marked with the
 * transaction retiring it, and a later keep lets go of its pages once that
 * transaction is known to have committed (no first use reads it from the
 * refs after), holding the relations against first uses still making
 * copies of it.  A commit kept again (a restore, a commit kept twice) is
 * retired too, never changed in place, so the pages it keeps stay until
 * the change is known to commit.
 */
int			prolly_kept_commits = 2;
int			prolly_restore_min_changes = 2;
int			prolly_root_interval = -1;
int			prolly_root_interval_commits = 1;


typedef struct KeptTable
{
	char	   *name;			/* qualified, as tables() names it */
	ProllyHash	data;			/* its data tree at the commit */
	int			nrels;
	BranchKeptRelation *rels;	/* the table's first */
} KeptTable;

typedef struct KeptCommit
{
	FullTransactionId retired;	/* by whom, its pages to be let go of
								 * (invalid: kept) */
	FullTransactionId kept_by;	/* who kept them, having seen every writer
								 * of the pages end */
	TimestampTz kept_at;		/* when */
	ProllyHash	commit;
	ProllyHash	baseline;		/* the branch's then (len 0: none) */
	int			ntables;
	KeptTable  *tables;
} KeptCommit;

#define KEPT_RELATION_SIZE	(5 * sizeof(uint32) + sizeof(uint64))

static bool commit_height(const ProllyHash *id, int64 *height);

/*
 * Roots are captured at commits, but no more often than the interval asks:
 * a commit is kept only once prolly.root_interval has passed since the
 * branch's newest kept commit was kept and prolly.root_interval_commits
 * commits have been made since it.  By default (-1) the interval is 0
 * (every commit) for commits made by hand, and a second under
 * dolt.transaction_commit, whose every transaction is a commit.  A move to
 * a commit that wasn't kept restores the nearest kept commit before it
 * and writes the rows the two differ by, the rows changed in an interval
 * (and what a merge between brought in).  The time a branch's root was last
 * kept, or a keep of it last tried, is shared by every backend
 * (branch_root_soon()): within the interval, a commit asks nothing of the
 * refs and tries no keep, and once it has passed, one backend's commit
 * claims the next try, not each backend's.
 */
static int
root_interval_ms(void)
{
	const char *tc;

	if (prolly_root_interval >= 0)
		return prolly_root_interval;
	tc = GetConfigOption("dolt.transaction_commit", true, false);
	return tc != NULL && strcmp(tc, "on") == 0 ? 1000 : 0;
}

/*
 * The session's branch (name, which is current: roots are kept of it
 * alone).
 */
static Oid
root_branch(const char *name)
{
	Assert(strcmp(name, branch_ops()->current()) == 0);
	return MyBranch;
}

/*
 * Is a commit of branch name too soon after its last root to keep?  With
 * claim, a commit that isn't is the one to try, as no other within the
 * interval will.
 */
static bool
root_soon(const char *name, bool claim)
{
	int			ms = root_interval_ms();

	return ms > 0 && OidIsValid(root_branch(name)) &&
		branch_root_soon(root_branch(name), ms, claim);
}

static void
root_note(const char *name, TimestampTz at)
{
	if (root_interval_ms() > 0 && OidIsValid(root_branch(name)))
		branch_root_note(root_branch(name), at);
}

/*
 * Is a root of branch name due at commit_id, its newest kept commit being
 * newest (NULL: none)?  The interval since it was kept, and the commits
 * made since (by height), each at least the settings'.
 */
static bool
root_due(const char *name, const KeptCommit *newest, const ProllyHash *commit_id)
{
	int			ms = root_interval_ms();
	int64		h;
	int64		hn;

	if (newest == NULL)
		return true;
	root_note(name, newest->kept_at);
	if (ms > 0 &&
		!TimestampDifferenceExceeds(newest->kept_at, GetCurrentTimestamp(), ms))
		return false;
	if (prolly_root_interval_commits > 1 &&
		commit_height(commit_id, &h) && commit_height(&newest->commit, &hn) &&
		h - hn < prolly_root_interval_commits)
		return false;
	return true;
}

static void
kept_key(StringInfo key, const char *name)
{
	appendStringInfoChar(key, TAG_KEPT);
	appendStringInfoString(key, name);
}

static void
kept_encode(StringInfo v, List *kept)
{
	KeptCommit *oldest = NULL;
	int64		h = -1;

	/* (the oldest not retired's height first, read alone: kept_lowest()) */
	foreach_ptr(KeptCommit, kc, kept)
	{
		if (!FullTransactionIdIsValid(kc->retired))
			oldest = kc;
	}
	if (oldest != NULL && !commit_height(&oldest->commit, &h))
		h = -1;
	appendBinaryStringInfo(v, &h, sizeof(h));
	foreach_ptr(KeptCommit, kc, kept)
	{
		appendBinaryStringInfo(v, &kc->retired, sizeof(FullTransactionId));
		appendBinaryStringInfo(v, &kc->kept_by, sizeof(FullTransactionId));
		appendBinaryStringInfo(v, &kc->kept_at, sizeof(TimestampTz));
		put_hash(v, &kc->commit);
		put_hash(v, &kc->baseline);
		prolly_put_varint(v, kc->ntables);
		for (int t = 0; t < kc->ntables; t++)
		{
			KeptTable  *kt = &kc->tables[t];

			prolly_put_varint(v, strlen(kt->name));
			appendStringInfoString(v, kt->name);
			put_hash(v, &kt->data);
			prolly_put_varint(v, kt->nrels);
			for (int r = 0; r < kt->nrels; r++)
			{
				BranchKeptRelation *k = &kt->rels[r];

				appendBinaryStringInfo(v, &k->relid, sizeof(uint32));
				appendBinaryStringInfo(v, &k->tablespace, sizeof(uint32));
				appendBinaryStringInfo(v, &k->relfilenumber, sizeof(uint32));
				appendBinaryStringInfo(v, &k->snapshot, sizeof(uint64));
				appendBinaryStringInfo(v, &k->frozenxid, sizeof(uint32));
				appendBinaryStringInfo(v, &k->minmxid, sizeof(uint32));
			}
		}
	}
}

static void
kept_corrupt(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("prolly refs hold an invalid record of kept pages")));
}

/* A TAG_KEPT entry's commits, newest first. */
static List *
kept_decode(const char *p, const char *end)
{
	List	   *kept = NIL;

	/* (past the height first: kept_encode()) */
	if (end - p < (ptrdiff_t) sizeof(int64))
		kept_corrupt();
	p += sizeof(int64);

	while (p < end)
	{
		KeptCommit *kc = palloc0(sizeof(KeptCommit));
		uint64		n;

		if (end - p < (ptrdiff_t) (2 * sizeof(FullTransactionId) +
								   sizeof(TimestampTz)))
			kept_corrupt();
		memcpy(&kc->retired, p, sizeof(FullTransactionId));
		p += sizeof(FullTransactionId);
		memcpy(&kc->kept_by, p, sizeof(FullTransactionId));
		p += sizeof(FullTransactionId);
		memcpy(&kc->kept_at, p, sizeof(TimestampTz));
		p += sizeof(TimestampTz);
		p = get_hash(p, end, &kc->commit);
		p = get_hash(p, end, &kc->baseline);
		n = prolly_get_varint(&p, end);
		if (n > (uint64) (end - p))
			kept_corrupt();
		kc->ntables = (int) n;
		kc->tables = palloc0_array(KeptTable, Max(kc->ntables, 1));
		for (int t = 0; t < kc->ntables; t++)
		{
			KeptTable  *kt = &kc->tables[t];

			kt->name = get_short_text(&p, end);
			if (kt->name == NULL)
				kept_corrupt();
			p = get_hash(p, end, &kt->data);
			n = prolly_get_varint(&p, end);
			if (n == 0 || n > (uint64) (end - p) / KEPT_RELATION_SIZE)
				kept_corrupt();
			kt->nrels = (int) n;
			kt->rels = palloc0_array(BranchKeptRelation, kt->nrels);
			for (int r = 0; r < kt->nrels; r++)
			{
				BranchKeptRelation *k = &kt->rels[r];

				memcpy(&k->relid, p, sizeof(uint32));
				memcpy(&k->tablespace, p + 4, sizeof(uint32));
				memcpy(&k->relfilenumber, p + 8, sizeof(uint32));
				memcpy(&k->snapshot, p + 12, sizeof(uint64));
				memcpy(&k->frozenxid, p + 20, sizeof(uint32));
				memcpy(&k->minmxid, p + 24, sizeof(uint32));
				p += KEPT_RELATION_SIZE;
			}
		}
		kept = lappend(kept, kc);
	}
	return kept;
}

/* A TAG_KEPT entry's commits and baselines, declared. */
static void
kept_declare(const char *p, const char *end, StringInfo refs)
{
	foreach_ptr(KeptCommit, kc, kept_decode(p, end))
	{
		PROLLY_CALL(tree, value_reference, &kc->commit, 0, refs);
		if (kc->baseline.len > 0)
			PROLLY_CALL(tree, value_reference, &kc->baseline, 0, refs);
		/* (each table's data tree, which the commit's root holds too) */
		for (int t = 0; t < kc->ntables; t++)
			if (kc->tables[t].data.len > 0)
				PROLLY_CALL(tree, value_reference, &kc->tables[t].data, 0, refs);
	}
}

/*
 * The index entries of branch name's kept commits kept, but those retired
 * (which no reader takes, and which pile up until their retiring
 * transaction is known to have committed): each commit's ('k'), and each
 * of its tables' data trees' ('s').  So they are a few for each branch,
 * however many commits the transaction makes.
 */
static List *
kept_index_keys(const char *name, List *kept)
{
	List	   *keys = NIL;

	foreach_ptr(KeptCommit, kc, kept)
	{
		StringInfo	k;

		if (FullTransactionIdIsValid(kc->retired))
			continue;
		k = makeStringInfo();
		appendStringInfoChar(k, TAG_KEPT_OF);
		put_hash(k, &kc->commit);
		appendStringInfoString(k, name);
		keys = lappend(keys, k);
		for (int t = 0; t < kc->ntables; t++)
		{
			if (kc->tables[t].data.len == 0)
				continue;
			k = makeStringInfo();
			appendStringInfoChar(k, TAG_KEPT_TABLE);
			put_hash(k, &kc->tables[t].data);
			put_hash(k, &kc->commit);
			appendStringInfoString(k, name);
			keys = lappend(keys, k);
		}
	}
	return keys;
}

static bool
key_listed(List *keys, const StringInfoData *key)
{
	foreach_ptr(StringInfoData, k, keys)
		if (k->len == key->len && memcmp(k->data, key->data, k->len) == 0)
			return true;
	return false;
}

/* The commits branch name's pages are kept as of, newest first. */
static List *
kept_get(const char *name)
{
	StringInfoData key;
	StringInfoData v;
	List	   *kept = NIL;
	MemoryContext old;

	initStringInfo(&key);
	initStringInfo(&v);
	kept_key(&key, name);
	if (refs_get(key.data, key.len, &v))
		kept = kept_decode(v.data, v.data + v.len);
	pfree(key.data);
	pfree(v.data);

	/* (its index entries now, for the kept_put() that follows) */
	if (kept_read_cxt == NULL)
		kept_read_cxt = AllocSetContextCreate(TopMemoryContext,
											  "prolly kept index entries read",
											  ALLOCSET_SMALL_SIZES);
	MemoryContextReset(kept_read_cxt);
	old = MemoryContextSwitchTo(kept_read_cxt);
	kept_read_name = pstrdup(name);
	kept_read_keys = kept_index_keys(name, kept);
	MemoryContextSwitchTo(old);
	return kept;
}

static void
kept_put(const char *name, List *kept)
{
	StringInfoData key;
	StringInfoData v;
	List	   *was;
	List	   *now = kept_index_keys(name, kept);

	/*
	 * (the entries as the last kept_get() found them, if it read this
	 * branch's and no kept_put() came since: the caller changes the list it
	 * read in place; else read again)
	 */
	if (kept_read_name == NULL || strcmp(kept_read_name, name) != 0)
		(void) kept_get(name);
	was = kept_read_keys;

	/* (the indexes first: those no longer true gone, the new ones put) */
	foreach_ptr(StringInfoData, k, was)
		if (!key_listed(now, k))
			index_put(k, false);
	foreach_ptr(StringInfoData, k, now)
		if (!key_listed(was, k))
			index_put(k, true);

	initStringInfo(&key);
	initStringInfo(&v);
	kept_key(&key, name);
	kept_encode(&v, kept);
	refs_put(key.data, key.len, kept != NIL ? &v : NULL);

	pfree(key.data);
	pfree(v.data);
	kept_read_name = NULL;
}

static KeptCommit *
kept_find(List *kept, const ProllyHash *commit)
{
	foreach_ptr(KeptCommit, kc, kept)
	{
		if (!FullTransactionIdIsValid(kc->retired) &&
			same_hash(&kc->commit, commit))
			return kc;
	}
	return NULL;
}

static void visit_prefix(const char *prefix, int prefixlen,
						 void (*visit) (const ProllyKey *k, const ProllyValue *v,
										void *arg),
						 void *arg);

typedef struct KeptHolder
{
	char	   *name;
	ProllyHash	commit;
} KeptHolder;

/* An index entry of kept_holders()'s prefix: its commit (if keyed by data) and name. */
static void
note_kept_holder(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	List	  **out = arg;
	const char *p = (const char *) k->data + 1;
	const char *end = (const char *) k->data + k->len;
	KeptHolder *h = palloc0(sizeof(KeptHolder));
	ProllyHash	skipped;

	if (k->data[0] == TAG_KEPT_TABLE)
		p = get_hash(p, end, &skipped);
	p = get_hash(p, end, &h->commit);
	h->name = pnstrdup(p, end - p);
	*out = lappend(*out, h);
}

static void
copies_kept_holders(const ProllyHash *commit, const ProllyHash *data,
					ProllyKeptHolderVisit visit, void *arg)
{
	StringInfoData prefix;
	List	   *holders = NIL;

	refs_init();
	initStringInfo(&prefix);
	appendStringInfoChar(&prefix, data != NULL ? TAG_KEPT_TABLE : TAG_KEPT_OF);
	put_hash(&prefix, data != NULL ? data : commit);
	/* (read whole first: a visit may read the refs of its own) */
	visit_prefix(prefix.data, prefix.len, note_kept_holder, &holders);
	pfree(prefix.data);
	foreach_ptr(KeptHolder, h, holders)
		visit(h->name, &h->commit, arg);
}

/* Visit the commits branch name's pages are kept as of, but those retired. */
static void
copies_kept_commits(const char *name, ProllyKeptCommitVisit visit, void *arg)
{
	foreach_ptr(KeptCommit, kc, kept_get(name))
	{
		ProllyKeptCommit k;
		ProllyKeptTable *tables;

		if (FullTransactionIdIsValid(kc->retired))
			continue;
		tables = palloc_array(ProllyKeptTable, Max(kc->ntables, 1));
		for (int t = 0; t < kc->ntables; t++)
		{
			tables[t].name = kc->tables[t].name;
			tables[t].nrels = kc->tables[t].nrels;
			tables[t].rels = kc->tables[t].rels;
		}
		k.commit = kc->commit;
		k.baseline = kc->baseline;
		k.kept_by = kc->kept_by;
		k.ntables = kc->ntables;
		k.tables = tables;
		visit(&k, arg);
	}
}

/* Does one of kept's commits keep relation k's pages by k's snapshot? */
static bool
kept_holds(List *kept, const BranchKeptRelation *k)
{
	foreach_ptr(KeptCommit, kc, kept)
	{
		for (int t = 0; t < kc->ntables; t++)
		{
			for (int r = 0; r < kc->tables[t].nrels; r++)
			{
				const BranchKeptRelation *o = &kc->tables[t].rels[r];

				/* (a root's id names it in its tablespace's store) */
				if (o->snapshot == k->snapshot &&
					o->tablespace == k->tablespace)
					return true;
			}
		}
	}
	return false;
}

static bool kept_is_storage(const KeptCommit *kc);
static bool kept_storage_unneeded(const KeptCommit *kc);

/*
 * May a retired commit's pages be let go of?  Once the transaction that
 * retired it committed, nobody reads it from the refs as kept any more,
 * but one may still be making a branch of them (make_kept()), which keep()
 * waits out.  The refs change with their transactions: a retiring the refs
 * show is one that committed, or this transaction's own (or one of its
 * subtransactions', whose ids follow its own).  So no transaction's status
 * is asked, which, long ago, may be gone.  One this transaction both
 * kept and retired (an earlier commit of the transaction's own) no other
 * ever saw, so it is let go of at once, outside any subtransaction:
 * otherwise a transaction making many commits would fill its relations'
 * snapshot slots with them, and its last commit, the head a reset gives
 * back, couldn't be kept.  (Inside one, the retiring may yet roll back,
 * bringing the commit back as kept, while a snapshot let go of stays so:
 * it waits for a keep at the top level, or the transaction's end.)
 * Storage a restore replaced (kept_is_storage()) is let go of only once
 * no snapshot is older than the move too (kept_storage_unneeded()).
 */
static bool
kept_releasable(const KeptCommit *kc)
{
	FullTransactionId top = GetTopFullTransactionIdIfAny();

	return FullTransactionIdIsValid(kc->retired) &&
		(!FullTransactionIdIsValid(top) ||
		 FullTransactionIdPrecedes(kc->retired, top) ||
		 (FullTransactionIdEquals(kc->retired, top) &&
		  FullTransactionIdEquals(kc->kept_by, top) &&
		  GetCurrentTransactionNestLevel() == 1)) &&
		(!kept_is_storage(kc) || kept_storage_unneeded(kc));
}

/*
 * Is retired entry kc the storage a restore replaced (replaced_entry()),
 * rather than a commit's pages?
 */
static bool
kept_is_storage(const KeptCommit *kc)
{
	return kc->ntables > 0 && kc->tables[0].nrels > 0 &&
		kc->tables[0].rels[0].snapshot == BRANCH_KEPT_STORAGE;
}

/*
 * Has every snapshot that didn't see the move replacing kc's storage
 * ended?  Every snapshot that doesn't see a transaction holds an xmin no
 * later than it (its own, or a running transaction's), so once the
 * oldest xmin any backend of any database holds is later, nothing reads
 * the storage again.  (After a restart none is older.)
 */
static bool
kept_storage_unneeded(const KeptCommit *kc)
{
	TransactionId horizon = GetOldestNonRemovableTransactionId(NULL);

	return FullTransactionIdPrecedes(kc->retired,
									 FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(),
																	  horizon));
}

/*
 * The storage a restore of table kt replaced (replaced[], each relation's
 * old storage, renewed[] its new), kept for the snapshots older than the
 * move: they go on reading it, as if the move hadn't been made, where the
 * restored pages would show them the commit's rows, neither their own nor
 * any the branch had while they were taken (a restore isn't otherwise
 * MVCC-safe, as TRUNCATE isn't).  It is a retired entry of the branch's
 * kept list, its relations naming the storage itself
 * (BRANCH_KEPT_STORAGE), so it is kept with the move's commit and dropped
 * (let go of, by keep()'s release[]) by the first keep after every
 * snapshot older than the move has ended (kept_releasable()); a crash
 * leaves it to the first keep after the restart.  The snapshots find it
 * through branch_note_replaced(), each relation's new storage naming its
 * old.
 */
static KeptCommit *
replaced_entry(const KeptCommit *kc, const KeptTable *kt,
			   const BranchKeptRelation *renewed,
			   const BranchKeptRelation *replaced)
{
	KeptCommit *o = palloc0(sizeof(KeptCommit));
	KeptTable  *t = palloc0(sizeof(KeptTable));
	FullTransactionId top = GetTopFullTransactionId();

	o->retired = top;
	o->kept_by = top;
	o->commit = kc->commit;
	o->ntables = 1;
	o->tables = t;
	t->name = pstrdup(kt->name);
	t->data = kt->data;
	t->rels = palloc_array(BranchKeptRelation, Max(kt->nrels, 1));
	for (int r = 0; r < kt->nrels; r++)
	{
		if (!RelFileNumberIsValid(replaced[r].relfilenumber))
			continue;
		t->rels[t->nrels++] = replaced[r];
		branch_note_replaced(replaced[r].tablespace,
							 replaced[r].relfilenumber,
							 renewed[r].relfilenumber, top);
	}
	return o;
}

/*
 * The relations stored with prolly table relid whose pages can be kept, its
 * own first; NIL if it isn't a prolly table.  Their locks, taken to read
 * them, are let go of again: keeping or restoring them takes its own, and
 * a lock held to the transaction's end would hold up a rewrite of a table
 * the transaction never otherwise touched.
 */
static List *
kept_relations(Oid relid)
{
	Relation	rel = try_relation_open(relid, AccessShareLock);
	bool		prolly;
	List	   *relids;

	if (rel == NULL)
		return NIL;
	prolly = rel->rd_rel->relam == PROLLY_TABLE_AM_OID &&
		rel->rd_rel->relkind == RELKIND_RELATION &&
		rel->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT;
	relation_close(rel, AccessShareLock);
	if (!prolly)
		return NIL;
	relids = stored_with(relid, NULL);
	foreach_oid(r, relids)
		UnlockRelationOid(r, AccessShareLock);
	return relids;
}

/*
 * The table a root names name (qualified), as the branch has it now;
 * InvalidOid if none.  Looked up whatever schemas the user may use: what
 * is kept is the branch's, not the user's.
 */
static Oid
table_named(const char *name)
{
	List	   *parts = stringToQualifiedNameList(name, NULL);
	Oid			nsp;

	if (list_length(parts) != 2 ||
		!OidIsValid(nsp = get_namespace_oid(strVal(linitial(parts)), true)))
		return InvalidOid;
	return get_relname_relid(strVal(lsecond(parts)), nsp);
}

/*
 * May the user give table relid its pages back?  What writing its rows
 * would take (the row applier's checks): every privilege to write it, and
 * UPDATE and DELETE on each table whose foreign keys reference it (their
 * checks are skipped as a move's are), none of them under row-level
 * security.  If not, the move writes its rows, and refuses as that does.
 */
static bool
may_restore(Oid relid)
{
	const AclMode write = ACL_SELECT | ACL_INSERT | ACL_UPDATE | ACL_DELETE;
	Relation	rel;
	TriggerDesc *td;
	bool		ok;

	if (pg_class_aclmask(relid, GetUserId(), write, ACLMASK_ALL) != write ||
		check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED)
		return false;
	/*
	 * (its lock let go of again, as kept_relations() lets go of its: a
	 * hard reset asks this before it takes the table's locks, all or none,
	 * and a lock kept here would be held while it waits for those)
	 */
	rel = relation_open(relid, AccessShareLock);
	td = rel->trigdesc;
	ok = true;
	for (int i = 0; ok && td && i < td->numtriggers; i++)
	{
		Oid			other = td->triggers[i].tgconstrrelid;

		if (RI_FKey_trigger_type(td->triggers[i].tgfoid) != RI_TRIGGER_PK ||
			other == relid)
			continue;
		ok = pg_class_aclmask(other, GetUserId(), ACL_UPDATE | ACL_DELETE,
							  ACLMASK_ALL) == (ACL_UPDATE | ACL_DELETE) &&
			check_enable_rls(other, InvalidOid, true) != RLS_ENABLED;
	}
	relation_close(rel, AccessShareLock);
	return ok;
}

/* Are relids just kt's relations (in any order)? */
static bool
same_relations(List *relids, const KeptTable *kt)
{
	if (list_length(relids) != kt->nrels)
		return false;
	for (int r = 0; r < kt->nrels; r++)
	{
		if (!list_member_oid(relids, kt->rels[r].relid))
			return false;
	}
	return true;
}

static bool
kept_settled(void *arg)
{
	return branch_writers_settled(MyBranch, (Snapshot) arg);
}

/*
 * What is left of kept commit kc's pages: restorable if some table's
 * relations all are (restore_pages() can give that table its pages back);
 * gone if nothing of any is left (each relation's storage, or its
 * snapshot there, gone: a restore, a TRUNCATE or a rewrite gave it new
 * storage, or it was dropped); else held (a snapshot is left to let go of,
 * but no table can be given its pages).  Without kept_status(), always
 * restorable, as restore() then tells.
 */
static BranchKeptStatus
kept_commit_status(const KeptCommit *kc)
{
	bool		any_left = false;

	if (branch_ops()->kept_status == NULL)
		return BRANCH_KEPT_RESTORABLE;
	for (int t = 0; t < kc->ntables; t++)
	{
		bool		all = true;

		for (int r = 0; r < kc->tables[t].nrels; r++)
		{
			BranchKeptStatus s = branch_ops()->kept_status(&kc->tables[t].rels[r]);

			pgstat_count_prolly(PGSTAT_PROLLY_KEPT_STATUS_CHECKS, 1);

			if (s != BRANCH_KEPT_RESTORABLE)
				all = false;
			if (s != BRANCH_KEPT_GONE)
				any_left = true;
			if (!all && any_left)
				break;
		}
		if (all)
			return BRANCH_KEPT_RESTORABLE;
	}
	return any_left ? BRANCH_KEPT_HELD : BRANCH_KEPT_GONE;
}

/*
 * The commits of kept (a branch's list) whose pages are gone dropped from
 * it, and those no table can be given its pages of any more retired: a
 * retired one keeps its commit and baseline from collection, as any does,
 * until a later keep lets go of its roots (a first use may be reading
 * it), a dropped one no longer.  Neither counts toward
 * prolly.kept_commits or is offered to a move or a first use.  A commit's
 * pages are gone with the change that replaced its storage (a rewrite, a
 * DROP), so the list changes with the same transaction; one whose pages
 * are gone has nothing for a first use reading the list from before to
 * make a branch of, and its roots, which its page store still keeps, are
 * let go of as that transaction commits.  (A TRUNCATE leaves a table's
 * change log on its storage: its commits are retired, not dropped.)  but:
 * an entry left as it is (NULL: none).
 *
 * Only the commits still kept are asked, no more of them than
 * prolly.kept_commits (and one a restore keeps again): a retired one is
 * neither counted nor offered whatever is left of its pages, and a later
 * keep lets go of what is (let go of, gone storage is passed over).  The
 * retired pile up within a transaction, each of its commits retiring one,
 * till it ends: were they asked too, a transaction's commits would each
 * ask of all the ones before.
 */
/* kept commits kept_drop_dead() asks of at most */
#define KEPT_ASKED_MAX	8

static List *
kept_drop_dead(List *kept, const KeptCommit *but)
{
	List	   *left = NIL;
	List	   *gone = NIL;
	int			asked = 0;

	foreach_ptr(KeptCommit, o, kept)
	{
		BranchKeptStatus s;

		/*
		 * (only the newest few: a commit's every table asked costs catalog
		 * reads, and a branch may keep thousands; one older found gone or
		 * held when a move asks of it is passed over then)
		 */
		if (o == but || FullTransactionIdIsValid(o->retired) ||
			asked >= KEPT_ASKED_MAX)
			s = BRANCH_KEPT_RESTORABLE;
		else
		{
			s = kept_commit_status(o);
			asked++;
		}

		if (s == BRANCH_KEPT_GONE)
		{
			gone = lappend(gone, o);
			continue;
		}
		if (s == BRANCH_KEPT_HELD && !FullTransactionIdIsValid(o->retired))
			o->retired = GetTopFullTransactionId();
		left = lappend(left, o);
	}

	/*
	 * (a gone commit's roots, which its page store keeps whatever became of
	 * the storage, let go of as the transaction commits, but those another
	 * commit left holds)
	 */
	if (branch_ops()->let_go)
		foreach_ptr(KeptCommit, o, gone)
			for (int t = 0; t < o->ntables; t++)
				for (int r = 0; r < o->tables[t].nrels; r++)
					if (!kept_holds(left, &o->tables[t].rels[r]))
						branch_ops()->let_go(&o->tables[t].rels[r]);
	return left;
}

/*
 * Keep the session branch's pages as of commit_id, whose root its working
 * set is known to be (snap's view, with the transaction's own changes).
 * Nothing is kept if a lock isn't free or another's writes may be on the
 * pages: a move back to the commit then writes its rows.
 */
static bool copies_commit_get(const ProllyHash *id, ProllyCommitInfo *info);

/*
 * Was kept commit o replaced by a commit made on parent (NULL: not known),
 * itself made on that parent?  (The parent itself, kept as a commit's
 * usually is, isn't: no commit is read for it.)
 */
static bool
kept_replaced(const KeptCommit *o, const ProllyHash *parent)
{
	ProllyCommitInfo was;

	return parent != NULL && !same_hash(&o->commit, parent) &&
		copies_commit_get(&o->commit, &was) && was.nparents > 0 &&
		same_hash(&was.parents[0], parent);
}

/* (parent: the commit's first parent, NULL if it has none or isn't known) */
static void
keep_pages(const ProllyHash *commit_id, const ProllyHash *parent,
		   const ProllyNodeRef *root, Snapshot snap)
{
	ProllyDatabaseComponents dc;
	char	   *name;
	List	   *tables = NIL;
	List	   *relids = NIL;
	List	   *groups = NIL;	/* each table's relids */
	List	   *names = NIL;
	List	   *kept;
	List	   *holding = NIL;
	List	   *before;
	List	   *releasing = NIL;
	List	   *next = NIL;
	BranchKeptRelation *prior;
	BranchKeptRelation *release;
	BranchKeptRelation *out;
	int			nprior = 0;
	int			nrelease = 0;
	int			live = 1;
	int			at = 0;
	KeptCommit *kc;
	ProllyNodeRef baseline;
	ListCell   *a;
	ListCell   *b;

	if (prolly_kept_commits <= 0 || branch_ops()->keep == NULL ||
		snap == NULL || RecoveryInProgress())
		return;
	name = branch_ops()->current();
	if (root_soon(name, true))
		return;
	INJECTION_POINT("prolly-root-kept-read", NULL);
	kept = kept_get(name);
	{
		KeptCommit *newest = NULL;

		foreach_ptr(KeptCommit, o, kept)
		{
			if (!FullTransactionIdIsValid(o->retired))
			{
				newest = o;
				break;
			}
		}
		if (!root_due(name, newest, commit_id))
			return;
	}
	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, tables, root, NULL, note_table, &tables);
	foreach_ptr(NamedTree, t, tables)
	{
		Oid			relid;
		List	   *g;

		if (!t->readable || !OidIsValid(relid = table_named(t->name)) ||
			(g = kept_relations(relid)) == NIL)
			continue;
		groups = lappend(groups, g);
		names = lappend(names, t);
		relids = list_concat(relids, g);
	}

	/*
	 * (the pages of retired commits now let go of are kept by no commit
	 * after: those the others hold stay)
	 */
	foreach_ptr(KeptCommit, o, kept)
	{
		if (kept_releasable(o))
			releasing = lappend(releasing, o);
		else
			holding = lappend(holding, o);
	}
	/*
	 * (a commit dropped as gone still holds what it shares with one let go
	 * of now: it is gone as this transaction sees the branch, and the
	 * change that made it so may yet roll back, bringing it back, while a
	 * snapshot let go of stays so)
	 */
	before = holding;
	holding = kept_drop_dead(holding, NULL);
	foreach_ptr(KeptCommit, o, holding)
	{
		for (int t = 0; t < o->ntables; t++)
			nprior += o->tables[t].nrels;
	}
	prior = palloc_array(BranchKeptRelation, Max(nprior, 1));
	nprior = 0;

	/*
	 * (a relation unchanged since one of the newest few kept commits
	 * reuses its root; asking each of thousands would cost a read of every
	 * root for every relation)
	 */
	foreach_ptr(KeptCommit, o, holding)
	{
		if (foreach_current_index(o) >= KEPT_ASKED_MAX)
			break;
		for (int t = 0; t < o->ntables; t++)
			for (int r = 0; r < o->tables[t].nrels; r++)
				prior[nprior++] = o->tables[t].rels[r];
	}
	foreach_ptr(KeptCommit, o, releasing)
	{
		for (int t = 0; t < o->ntables; t++)
			nrelease += o->tables[t].nrels;
	}
	release = palloc_array(BranchKeptRelation, Max(nrelease, 1));
	nrelease = 0;
	foreach_ptr(KeptCommit, o, releasing)
	{
		for (int t = 0; t < o->ntables; t++)
			for (int r = 0; r < o->tables[t].nrels; r++)
				if (!kept_holds(before, &o->tables[t].rels[r]))
					release[nrelease++] = o->tables[t].rels[r];
	}
	out = palloc0_array(BranchKeptRelation, Max(list_length(relids), 1));
	/*
	 * (a keep that can't be had now, a writer's lock held, isn't tried
	 * again within the interval: under dolt.transaction_commit, every
	 * transaction's commit would try it)
	 */
	if (!branch_ops()->keep(relids, prior, nprior, out, release, nrelease,
							kept_settled, snap))
	{
		root_note(name, GetCurrentTimestamp());
		return;
	}

	kc = palloc0(sizeof(KeptCommit));
	kc->kept_by = GetTopFullTransactionId();
	kc->kept_at = GetCurrentTimestamp();
	kc->commit = *commit_id;
	if (branch_baseline(name, &baseline))
		kc->baseline = baseline.hash;
	kc->ntables = list_length(groups);
	kc->tables = palloc0_array(KeptTable, Max(kc->ntables, 1));
	forboth(a, groups, b, names)
	{
		KeptTable  *kt = &kc->tables[foreach_current_index(a)];

		kt->name = ((NamedTree *) lfirst(b))->name;
		kt->data = ((NamedTree *) lfirst(b))->data.hash;
		kt->nrels = list_length(lfirst(a));
		kt->rels = &out[at];
		at += kt->nrels;
	}

	/*
	 * (the commit first, then the others kept till there are enough; the
	 * rest retired, let go of by a later keep once this transaction
	 * commits, as a branch may be being made of them meanwhile.  One the
	 * commit replaced, made on the same parent, is retired too: amended
	 * away, or reworded or squashed by a rebase, it is no longer the
	 * branch's, and kept it would push out the parent a reset goes back to)
	 */
	next = list_make1(kc);
	foreach_ptr(KeptCommit, o, holding)
	{
		if (FullTransactionIdIsValid(o->retired))
			next = lappend(next, o);
		else if (!same_hash(&o->commit, commit_id) && live < prolly_kept_commits &&
				 !kept_replaced(o, parent))
		{
			next = lappend(next, o);
			live++;
		}
		else
		{
			o->retired = GetTopFullTransactionId();
			next = lappend(next, o);
		}
	}
	kept_put(name, next);
	root_note(name, kc->kept_at);
}

/*
 * Giving a table its kept pages back costs a constant (new storage for
 * each of its relations, its catalog row and caches), while writing its
 * rows back costs each row its pages, which the commit's pages kept again
 * after the move then sync: at 1M rows, with fsync on, ~11 ms restored
 * against ~7 ms for one row written, ~11 ms for four and ~20 ms for eight.
 * So fewer than prolly.restore_min_changes changed since a baseline
 * holding the commit's rows are written instead.
 */

/*
 * Would writing table relid's rows back to the commit's (data) cost less
 * than restoring its pages: its baseline holds the commit's rows, and
 * fewer than prolly.restore_min_changes of them changed since, as its
 * change finder tells from no more than that many of its records?
 */
static bool
few_changes(const ProllyDatabaseComponents *dc, const ProllyNodeRef *baseline,
			Oid relid, const ProllyNodeRef *data)
{
	ProllyNodeRef base;

	if (prolly_restore_min_changes <= 0 ||
		!PROLLY_HAS(dc->canonicalizer, table_fewer_changes) ||
		!dc->canonicalizer->table_fewer_changes(baseline, relid,
												prolly_restore_min_changes,
												&base))
		return false;
	return same_hash(&base.hash, &data->hash);
}

/*
 * The relations of table name as the other commits of kept (but was, and
 * those retired) keep them: copies into *carry, and where each is into
 * *into, for a restore of the table to carry their snapshots into the
 * storage it gives it (restore()'s carry[]), which would otherwise go with
 * the storage it replaces.  How many.
 */
static int
kept_carried(List *kept, const struct KeptCommit *was, const char *name,
			 BranchKeptRelation **carry, BranchKeptRelation ***into)
{
	int			n = 0;

	foreach_ptr(KeptCommit, o, kept)
	{
		if (o == was || FullTransactionIdIsValid(o->retired))
			continue;
		for (int t = 0; t < o->ntables; t++)
			if (strcmp(o->tables[t].name, name) == 0)
				n += o->tables[t].nrels;
	}
	*carry = palloc_array(BranchKeptRelation, Max(n, 1));
	*into = palloc_array(BranchKeptRelation *, Max(n, 1));
	n = 0;
	foreach_ptr(KeptCommit, o, kept)
	{
		if (o == was || FullTransactionIdIsValid(o->retired))
			continue;
		for (int t = 0; t < o->ntables; t++)
		{
			if (strcmp(o->tables[t].name, name) != 0)
				continue;
			for (int r = 0; r < o->tables[t].nrels; r++)
			{
				(*carry)[n] = o->tables[t].rels[r];
				(*into)[n] = &o->tables[t].rels[r];
				n++;
			}
		}
	}
	return n;
}

/*
 * Would restore_kept() give table kt (of a kept commit whose readable
 * tables are readable) its kept pages, its relation *relid: it is on the
 * branch as it was then (other: kept by another branch, whose pages it
 * may share), the user may write it, more than a few of its rows
 * changed since baseline (few_changes()), and, for the head's pages
 * (moving: targets the move's commit's tables), the move's commit holds
 * it?  Asked by the restore, and before a hard reset holds the refs, of
 * the tables it will lock for it (lock_for_restore()).
 */
static bool
restore_planned(const ProllyDatabaseComponents *dc, const KeptTable *kt,
				List *readable, List *targets, bool moving,
				const ProllyNodeRef *baseline, const KeptCommit *other,
				Oid *relid)
{
	NamedTree  *target = NULL;
	NamedTree  *moved = NULL;

	*relid = table_named(kt->name);
	foreach_ptr(NamedTree, r, readable)
	{
		if (strcmp(r->name, kt->name) == 0)
			target = r;
	}
	foreach_ptr(NamedTree, r, targets)
	{
		if (strcmp(r->name, kt->name) == 0)
			moved = r;
	}
	return target != NULL &&
		OidIsValid(*relid) && *relid == kt->rels[0].relid &&
		(other != NULL ?
		 branch_ops()->share_kept != NULL &&
		 kept_before_every_snapshot_xid(other->kept_by) &&
		 same_relation_set(kept_relations(*relid), kt->rels, kt->nrels) :
		 same_relations(kept_relations(*relid), kt)) &&
		may_restore(*relid) &&
		!few_changes(dc, baseline, *relid, &target->data) &&
		!(moving && moved == NULL);
}

/*
 * Give the session branch's tables back the pages kept as of commit
 * (was, in the branch's list kept), but those named in skip: each that
 * changed since (O(1) each), whose baseline entry is then the kept one's.
 * A table not on the branch as it was then (another name, other
 * relations, storage replaced since), or not restored (its snapshot gone,
 * the session itself using it), is left for the move to write, as is one
 * with so few rows changed that writing them costs less (few_changes()),
 * or, with moving_to (the move's commit, when commit is the head), one the
 * move's commit doesn't hold (the move drops it).  With moving_to, every
 * other table that changed since the head is given the head's pages
 * whatever the working set holds: discarding its changes so is O(1),
 * where writing the rows back would cost each row changed, and the move
 * then writes only the rows the head and the commit differ by.  The
 * storage each restored relation had is kept for the snapshots older than
 * the move (replaced_entry()).  The tables restored are added to
 * *restored, and *baseline spliced with the kept one's entries for them;
 * *kept is the branch's list with the commit kept anew (by the restored
 * tables' new storage) and the storage replaced, to be put once the
 * restores are done.
 */
static void
restore_kept(const ProllyDatabaseComponents *dc, const ProllyCommitInfo *commit,
			 KeptCommit *was, Oid source, List **kept, List *skip,
			 const ProllyNodeRef *moving_to, ProllyNodeRef *baseline,
			 List **restored)
{
	bool		other = OidIsValid(source) && source != MyBranch;


	KeptCommit *kc;
	List	   *tables = NIL;
	List	   *readable = NIL;
	List	   *targets = NIL;
	List	   *now = NIL;
	List	   *own = NIL;		/* tables another branch's pages were held of */
	List	   *storage = NIL;
	ProllyNodeRef from;
	ProllyNodeRef spliced;

	/*
	 * (another branch's pages only where the commit defines what the
	 * branch's head does: its types are another's objects, their values on
	 * the pages read by this branch's, which the move then makes again)
	 */
	if (other)
	{
		ProllyHash	head;
		ProllyCommitInfo hi;

		if (dc->canonicalizer->same_schema == NULL ||
			!branch_head(branch_ops()->current(), &head) ||
			!copies_commit_get(&head, &hi) ||
			!dc->canonicalizer->same_schema(&hi.root, &commit->root))
			return;
	}

	/*
	 * (the commit kept anew, the restored tables by their new storage's
	 * snapshots; the entry it was kept by retired, never changed in place,
	 * so its snapshots stay until the transaction is known to commit)
	 */
	kc = palloc(sizeof(KeptCommit));
	*kc = *was;
	kc->kept_by = GetTopFullTransactionId();
	kc->tables = palloc_array(KeptTable, Max(was->ntables, 1));
	for (int t = 0; t < was->ntables; t++)
	{
		kc->tables[t] = was->tables[t];
		kc->tables[t].rels = palloc_array(BranchKeptRelation, was->tables[t].nrels);
		memcpy(kc->tables[t].rels, was->tables[t].rels,
			   sizeof(BranchKeptRelation) * was->tables[t].nrels);
	}

	/*
	 * (only tables whose rows the commit holds as this database can read
	 * them: the move refuses the others, as reading their rows would)
	 */
	PROLLY_CALL(dc->canonicalizer, tables, &commit->root, NULL, note_table,
				&tables);
	foreach_ptr(NamedTree, t, tables)
	{
		if (t->readable)
			readable = lappend(readable, t);
	}
	if (moving_to != NULL)
		PROLLY_CALL(dc->canonicalizer, tables, moving_to, NULL, note_table,
					&targets);
	for (int t = 0; t < kc->ntables; t++)
	{
		KeptTable  *kt = &kc->tables[t];
		Oid			relid;
		BranchKeptRelation *renewed;
		BranchKeptRelation *replaced;
		BranchKeptRelation *carry;
		BranchKeptRelation *carried;
		BranchKeptRelation **into;
		int			ncarry;
		bool		any = false;
		bool		skipped = false;

		foreach_ptr(char, n, skip)
		{
			if (strcmp(n, kt->name) == 0)
				skipped = true;
		}
		if (skipped)
			continue;
		if (!restore_planned(dc, kt, readable, targets, moving_to != NULL,
							 baseline, other ? was : NULL, &relid))
			continue;
		renewed = palloc_array(BranchKeptRelation, kt->nrels);
		replaced = palloc_array(BranchKeptRelation, kt->nrels);

		/*
		 * (another branch's kept pages: shared, as a move shares them,
		 * that branch's list left as it is, and the session's kept commits
		 * of the table, of another lineage, carried nowhere; the storage
		 * replaced kept for the snapshots older than the move, as a
		 * restore's is)
		 */
		if (other)
		{
			BranchKeptRelation *held;
			bool		all = branch_ops()->hold != NULL;
			int			nheld = 0;

			/*
			 * (the session's own kept commits of the table carried to the
			 * new storage, renamed, as a restore on the branch carries them:
			 * their roots are the store's)
			 */
			ncarry = kept_carried(*kept, NULL, kt->name, &carry, &into);
			carried = palloc_array(BranchKeptRelation, Max(ncarry, 1));
			if (!branch_replaced_reserve(kt->nrels))
				continue;
			if (!branch_ops()->share_kept(source, kt->rels, kt->nrels, carry,
										  ncarry, carried, renewed, replaced,
										  &move_wait))
			{
				branch_replaced_release_unused();
				continue;
			}
			for (int j = 0; j < ncarry; j++)
				*into[j] = carried[j];
			now = lappend(now, kt->name);
			storage = lappend(storage,
							  replaced_entry(kc, kt, renewed, replaced));
			branch_replaced_release_unused();
			pgstat_count_prolly(PGSTAT_PROLLY_MOVE_TABLES_SHARED, 1);

			/*
			 * (the pages held for the session's list too, by roots of its
			 * own, so that its next keep finds them unchanged and a later
			 * move restores them as its own; if one can't be, those held
			 * of the table's other relations let go of again)
			 */
			held = palloc_array(BranchKeptRelation, kt->nrels);
			for (int r = 0; r < kt->nrels && all; r++)
			{
				all = !(r > 0 && IS_INJECTION_POINT_ATTACHED("refs-hold-refused")) &&
					branch_ops()->hold(&kt->rels[r], &held[r]);
				if (all)
					nheld++;
			}
			if (all)
			{
				memcpy(kt->rels, held, sizeof(BranchKeptRelation) * kt->nrels);
				own = lappend(own, kt);
			}
			else if (branch_ops()->let_go != NULL)
				for (int r = 0; r < nheld; r++)
					branch_ops()->let_go(&held[r]);
			continue;
		}
		ncarry = kept_carried(*kept, was, kt->name, &carry, &into);
		carried = palloc_array(BranchKeptRelation, Max(ncarry, 1));
		/*
		 * (an entry for each relation's replaced storage reserved first:
		 * with none free, nothing is replaced, and the rows are written,
		 * so no snapshot older than the move reads storage it can't tell)
		 */
		if (!branch_replaced_reserve(kt->nrels))
			continue;
		if (!branch_ops()->restore(kt->rels, kt->nrels, renewed, carry, ncarry,
								   carried, replaced, &move_wait))
		{
			branch_replaced_release_unused();
			continue;
		}
		for (int j = 0; j < ncarry; j++)
			*into[j] = carried[j];
		for (int r = 0; r < kt->nrels; r++)
			any |= renewed[r].relfilenumber != kt->rels[r].relfilenumber;
		if (any)
			storage = lappend(storage, replaced_entry(kc, kt, renewed, replaced));
		branch_replaced_release_unused();
		if (!any)
			continue;
		memcpy(kt->rels, renewed, sizeof(BranchKeptRelation) * kt->nrels);
		now = lappend(now, kt->name);
	}
	if (now == NIL)
		return;

	/*
	 * (the restored tables' roots, which their new storage is of, kept at
	 * once; the other commits whose pages were in the storage replaced
	 * dropped with it)
	 */
	if (!other)
	{
		*kept = kept_drop_dead(lcons(kc, *kept), kc);
		was->retired = GetTopFullTransactionId();
	}
	else if (own != NIL)
	{
		/*
		 * (kept anew by the session's branch: only the tables it holds, as
		 * of now, for its own interval)
		 */
		int			t = 0;

		kc->kept_at = GetCurrentTimestamp();
		kc->ntables = list_length(own);
		foreach_ptr(KeptTable, kt, own)
			kc->tables[t++] = *kt;
		*kept = lcons(kc, *kept);
	}
	*kept = list_concat(*kept, storage);
	ref_of(&kc->baseline, &from);
	PROLLY_CALL(dc->canonicalizer, splice_baseline, baseline, &from, now,
				&spliced);
	*baseline = spliced;
	*restored = list_concat(*restored, now);
}

/* commits before a move's commit that the nearest kept one is looked for in */
#define NEAREST_KEPT_MAX	1024

/*
 * The nearest commit before commit (by first parents, as its branch made
 * them, at most NEAREST_KEPT_MAX back) whose pages kept (the branch's list)
 * keeps, its commit into *info: NULL if none.
 *
 * The walk stops below the height of the oldest commit any branch's list
 * keeps (each list's last entry not retired): each step costs a commit's
 * read and a look in the refs' index of every branch's kept commits, and
 * below that height no branch keeps a root.  (A branch made at another's
 * head, with a list of its own, still finds that branch's roots below its
 * own.)  First parents only:
 * the rows between are written as a diff of the two commits' trees,
 * whatever lies between, so a merge between costs the rows its other
 * parent brought in, and a root on that side, nearer, is not looked for.
 */
typedef struct OtherKept
{
	const char *current;
	char	   *name;			/* the first other branch keeping it */
} OtherKept;

static void
note_other_kept(const char *name, const ProllyHash *commit, void *arg)
{
	OtherKept  *ok = arg;

	if (ok->name == NULL && strcmp(name, ok->current) != 0 &&
		strlen(name) < NAMEDATALEN)
		ok->name = pstrdup(name);
}

/*
 * Commit id's kept pages, which the session branch's list (kept) has, or
 * else another branch's (found by the refs' index of kept commits): the
 * entry, and in *source the branch keeping it.  NULL if none does.  A
 * commit's pages are the commit's whichever branch kept them.
 */
static KeptCommit *
kept_anywhere(List *kept, const ProllyHash *id, Oid *source)
{
	KeptCommit *kc = kept_find(kept, id);
	OtherKept	ok = {branch_ops()->current(), NULL};
	HeapTuple	tup;

	*source = MyBranch;
	if (kc != NULL && kc->baseline.len > 0)
		return kc;
	copies_kept_holders(id, NULL, note_other_kept, &ok);
	if (ok.name == NULL)
		return NULL;
	kc = kept_find(kept_get(ok.name), id);
	if (kc == NULL || kc->baseline.len == 0)
		return NULL;
	tup = SearchSysCache1(BRANCHNAME, CStringGetDatum(ok.name));
	if (!HeapTupleIsValid(tup))
		return NULL;
	*source = ((Form_pg_branch) GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);
	return kc;
}

/*
 * The lowest height of the oldest commit any branch's list keeps (its last
 * entry not retired): -1 if none, or one's height isn't known.  (A visit of
 * the refs' TAG_KEPT entries, reading each one's first height alone.)
 */
typedef struct LowestKept
{
	int64		lowest;
} LowestKept;

static void
note_lowest_kept(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	LowestKept *lk = arg;
	int64		h;

	if (k->len == 0 || k->data[0] != TAG_KEPT)
		return;
	if (v->len < sizeof(h))
		kept_corrupt();
	memcpy(&h, v->data, sizeof(h));
	if (h < 0)
		return;
	if (lk->lowest < 0 || h < lk->lowest)
		lk->lowest = h;
}

static KeptCommit *
nearest_kept(List *kept, const ProllyCommitInfo *commit, ProllyCommitInfo *info,
			 Oid *source)
{
	ProllyHash	id;
	int64		lowest;
	LowestKept	lk = {-1};
	char		prefix = TAG_KEPT;

	if (commit->nparents == 0)
		return NULL;
	visit_prefix(&prefix, 1, note_lowest_kept, &lk);
	lowest = lk.lowest;
	id = commit->parents[0];
	for (int n = 0; n < NEAREST_KEPT_MAX; n++)
	{
		KeptCommit *kc;

		if (!copies_commit_get(&id, info))
			return NULL;
		if (lowest >= 0 && info->has_height && info->height < lowest)
			return NULL;
		if ((kc = kept_anywhere(kept, &id, source)) != NULL)
			return kc;
		if (info->nparents == 0)
			return NULL;
		id = info->parents[0];
	}
	return NULL;
}

/*
 * Before a move of the session's branch to commit: if its pages are kept
 * as of the commit, each table's given back (restore_kept()).  If not
 * (roots are captured an interval apart), the pages kept as of the
 * nearest kept commit before it are, where they pay, and the move then
 * writes the rows the two differ by.  The tables
 * that aren't (the commit isn't kept, or a table's kept pages are gone: a
 * restore of another commit since gave the table new storage, and its
 * old storage, where the commit's snapshot was, went with it) are given
 * the pages kept as of the branch's head, if those are kept: so the move
 * discards the working set's changes in O(1), as a hard reset to the head
 * does, and writes only the rows the head and the commit differ by, not
 * those and every row changed since the head.  (A move always ends at the
 * commit's rows, so whatever the working set held is the move's to
 * discard.)
 */
static void
restore_pages(const ProllyCommitInfo *commit)
{
	ProllyDatabaseComponents dc;
	char	   *name;
	List	   *kept;
	KeptCommit *was;
	List	   *restored = NIL;
	ProllyNodeRef baseline;
	ProllyHash	head;
	ProllyCommitInfo at_head;
	Oid			source;

	if (prolly_kept_commits <= 0 || branch_ops()->restore == NULL ||
		RecoveryInProgress())
		return;
	prolly_database_components(&dc);
	if (dc.canonicalizer->splice_baseline == NULL)
		return;
	name = branch_ops()->current();
	kept = kept_get(name);
	if (!branch_baseline(name, &baseline))
		return;
	was = kept_anywhere(kept, &commit->id, &source);
	if (was != NULL)
		restore_kept(&dc, commit, was, source, &kept, NIL, NULL, &baseline,
					 &restored);

	/*
	 * (another branch's roots of the commit, of a schema the session's
	 * branch no longer has, restore nothing: the nearest before it may)
	 */
	if (was != NULL && source != MyBranch && restored == NIL)
		was = NULL;
	if (was == NULL &&
		(was = nearest_kept(kept, commit, &at_head, &source)) != NULL &&
			 !(branch_head(name, &head) && same_hash(&head, &at_head.id)))
	{
		int			before = list_length(restored);

		/*
		 * (as the head's below: the rows between written after, so a
		 * snapshot older than the move must not read the pages meanwhile)
		 */
		restore_kept(&dc, &at_head, was, source, &kept, restored,
					 &commit->root, &baseline, &restored);
		if (list_length(restored) > before)
			branch_note_head_restore(MyBranch);
	}
	if (branch_head(name, &head) && !same_hash(&head, &commit->id) &&
		(was = kept_find(kept, &head)) != NULL && was->baseline.len > 0 &&
		copies_commit_get(&head, &at_head))
	{
		int			before = list_length(restored);

		restore_kept(&dc, &at_head, was, MyBranch, &kept, restored,
					 &commit->root, &baseline, &restored);
		if (list_length(restored) > before)
			branch_note_head_restore(MyBranch);
	}
	if (restored == NIL)
		return;
	kept_put(name, kept);
	branch_set_baseline(name, &baseline, NULL);
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
	scan = catalog_branch_beginscan(classes, MyBranch);
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
static void move_copies_checked(void *arg);

typedef struct MoveCopies
{
	const ProllyCommitInfo *commit;
	const KnownSource *source;
	ProllyHash	head;			/* the commit moved to */
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
			settle(mc->commit, NULL);
		}
		else
			settle(mc->commit, mc->source);
		return;
	}
	PushActiveSnapshot(GetLatestSnapshot());
	settle(mc->commit, mc->source);
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

static void
note_root_of(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	List	  **names = arg;
	int			len;
	const char *name = entry_branch((const char *) k->data, k->len, &len);

	if (name != NULL)
		*names = lappend(*names, pnstrdup(name, len));
}

/*
 * The branches with copies that could be nearest root (rank_source()):
 * those a fold of is known, and those whose head's or baseline's fold's
 * root it is (the refs' index of roots), each once, in name order, as a
 * listing of every branch would visit them.  Any other ranks 0.
 */
static List *
root_candidates(const ProllyHash *root)
{
	List	   *out = known_fold_branches();
	List	   *names = NIL;
	StringInfoData prefix;

	initStringInfo(&prefix);
	for (int kind = 0; kind < 2; kind++)
	{
		resetStringInfo(&prefix);
		appendStringInfoChar(&prefix, TAG_ROOT_OF);
		put_hash(&prefix, root);
		appendStringInfoChar(&prefix, kind == 0 ? 'w' : 'h');
		visit_prefix(prefix.data, prefix.len, note_root_of, &names);
	}
	pfree(prefix.data);
	foreach_ptr(char, name, names)
	{
		bool		listed = false;
		NamedBranch *nb;
		Oid			branch;

		foreach_ptr(NamedBranch, x, out)
			if (strcmp(x->name, name) == 0)
				listed = true;
		if (listed || !OidIsValid(branch = branch_oid(name)))
			continue;
		nb = palloc(sizeof(NamedBranch));
		nb->name = name;
		nb->branch = branch;
		out = lappend(out, nb);
	}
	list_sort(out, named_branch_cmp);
	return out;
}

/*
 * The kept commit a first use is made of (the materialization cache's,
 * whose kept commits the move's shares and the planner's costs take too):
 * one at the branch's head, the session's branch's if it keeps it.
 */
typedef struct KeptSource
{
	const ProllyHash *commit;
	const ProllyKeptState *state;
} KeptSource;

static void
find_kept_commit(const ProllyKeptState *state, void *arg)
{
	KeptSource *ks = arg;

	if (state->kind != PROLLY_KEPT_COMMIT ||
		!same_hash(&state->commit, ks->commit) ||
		(ks->state != NULL && state->branch != MyBranch))
		return;
	ks->state = state;
}

/*
 * With no branch at the root: the branch whose copies would cost least to
 * move there, as the move planner estimates the move from what its working
 * set is known or thought to be (its working set known, else its
 * baseline's fold, else its head; one changed since is only a hint, the
 * move folding the copies as they are).  The session's branch is planned
 * first and kept on a tie or near one (SOURCE_COST_MARGIN); at most
 * SOURCE_CANDIDATES others are planned
 * after it, each stopped once it costs more than the best yet, each planned
 * on its own branch (its tables' indexes are its own).  NULL if
 * none is cheaper than the session's branch.  A kept materialization is a
 * branch, so a candidate by the root it holds; and each plan is costed with
 * the tables any kept one holds as the target does shared, not written
 * (cost_with_shares()), as the move then shares them (share_clean_tables():
 * an emptied copy too), so the copies chosen are those cheapest to finish
 * with the shares, not without them; and then, with tables to share, a
 * candidate isn't stopped at the best's cost.  The copies are new in the
 * transaction, which no other sees: the tables of the chosen plan that
 * cost less emptied and loaded than moved row by row (#713), and those the
 * head hasn't, which the move drops (#714), are added to *emptied, for
 * their copies to begin empty: no page of theirs is shared only to be
 * written over or dropped.  Those of the session's branch's plan go to
 * *own_emptied, for the copies made of it instead if the chosen branch is
 * busy.
 */
#define SOURCE_CANDIDATES	16

/*
 * A candidate is taken over the best yet only where it costs clearly less:
 * a diff past the planner's node bound is estimated from a sample, off
 * by up to a tenth or so either way, so two candidates as far from the head (the
 * session's branch and another at the same commit, say) can cost a little
 * apart, and the session's branch, which wins a tie, must win those too.
 */
#define SOURCE_COST_MARGIN	0.8

/* Is branch name a kept materialization (the materialization cache's)? */
static bool
branch_kept(const char *name)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.matcache != NULL && PROLLY_HAS(dc.matcache, kept) &&
		PROLLY_CALL(dc.matcache, kept, name);
}

typedef struct SourceCandidates
{
	Snapshot	snap;
	bool		others;			/* the others noted now, not the session's */
	List	   *kept_seen;		/* OIDs of the kept materializations snap
								 * sees made: no other kept one is a
								 * candidate */
	List	   *names;			/* char *, the session's branch first */
	List	   *roots;			/* ProllyNodeRef *, what each is thought to be */
} SourceCandidates;

/*
 * What branch name's working set is thought to be, as snap would see it:
 * known, else its baseline's fold, else its head (false: none of them).
 */
static bool
thought_root(const char *name, Oid branch, Snapshot snap, ProllyNodeRef *root)
{
	ProllyNodeRef baseline;
	ProllyHash	h;
	ProllyCommitInfo info;
	BranchFoldStamp at;

	if (fold_caches_usable() &&
		branch_fold_known_at(branch, snap,
							 branch_baseline(name, &baseline) ? &baseline.hash : NULL,
							 false, &h, &at))
		ref_of(&h, root);
	else if (branch_baseline_root(name, root))
		;
	else if (branch_head(name, &h) && copies_commit_get(&h, &info))
		*root = info.root;
	else
		return false;
	return true;
}

static void
note_candidate(const char *name, Oid branch, void *arg)
{
	SourceCandidates *sc = arg;
	ProllyNodeRef *root = palloc(sizeof(ProllyNodeRef));

	if ((branch != MyBranch) != sc->others ||
		list_length(sc->names) > SOURCE_CANDIDATES)
		return;
	/* (one the snapshot doesn't see made holds rows it can't see) */
	if (sc->others && !list_member_oid(sc->kept_seen, branch) &&
		branch_kept(name))
		return;
	if (!thought_root(name, branch, sc->snap, root))
		return;
	sc->names = lappend(sc->names, pstrdup(name));
	sc->roots = lappend(sc->roots, root);
}

/* note_candidate(), until enough are noted. */
static bool
note_candidate_while(const char *name, Oid branch, void *arg)
{
	SourceCandidates *sc = arg;

	note_candidate(name, branch, arg);
	return list_length(sc->names) <= SOURCE_CANDIDATES;
}

/* The tables of a plan whose copies begin empty, added to *emptied. */
static void
reloaded_tables(const ProllyMovePlan *plan, List **emptied)
{
	ListCell   *lc;

	foreach(lc, plan->tables)
	{
		ProllyTableMove *m = lfirst(lc);

		if (m->kind == PROLLY_TABLE_RELOAD || m->kind == PROLLY_TABLE_DROPPED)
			*emptied = lappend(*emptied, m->name);
	}
}

/* A plan made on a candidate's branch (run_on()). */
typedef struct PlanOn
{
	const ProllyMovePlannerRoutine *planner;
	const ProllyNodeRef *from;
	const ProllyNodeRef *to;
	double		bound;
	ProllyMovePlan *plan;
} PlanOn;

static void
plan_here(void *arg)
{
	PlanOn	   *po = arg;

	PROLLY_CALL(po->planner, plan, po->from, po->to, PROLLY_PLAN_RELOADS,
				po->bound, po->plan);
}

/*
 * The target's tables a kept materialization holds as the target has them
 * (the materialization cache's, made before snap): a first use's move
 * shares each of these instead of writing its rows (share_clean_tables()),
 * where its definitions are the target's.  Their names, as Strings; and
 * the kept materializations snap sees made, into *seen.
 */
static List *
kept_shareable(const ProllyNodeRef *target, Snapshot snap, List **seen)
{
	ProllyDatabaseComponents dc;
	ShareSources ss = {snap, NIL};
	List	   *theirs = NIL;
	List	   *names = NIL;
	ProllyKeptFilter filter;

	prolly_database_components(&dc);
	if (dc.matcache == NULL || !PROLLY_HAS(dc.matcache, visit))
		return NIL;
	PROLLY_CALL(dc.canonicalizer, tables, target, NULL, note_table, &theirs);
	kept_filter_of_tables(theirs, &filter);
	PROLLY_CALL(dc.matcache, visit, snap, &filter, find_kept_source, &ss);
	foreach_ptr(ShareSource, s, ss.sources)
		*seen = lappend_oid(*seen, s->branch);
	if (ss.sources == NIL)
		return NIL;
	foreach_ptr(NamedTree, t, theirs)
		if (source_of(ss.sources, t) != NULL)
			names = lappend(names, makeString(t->name));
	return names;
}

/*
 * A plan's cost with the tables a kept materialization holds as shared
 * instead of written: each costs no more than sharing a table and the
 * relations stored with it.  Only a plan of rows alone (the definitions
 * alike), as a share is made only then.
 */
#define SHARE_TABLE_USEC	(4 * SHARE_RELATION_USEC)

static double
cost_with_shares(const ProllyMovePlan *plan, List *shareable)
{
	double		cost = plan->cost;

	if (shareable == NIL)
		return cost;
	foreach_ptr(ProllyTableMove, m, plan->tables)
		if (m->kind != PROLLY_TABLE_ROWS && m->kind != PROLLY_TABLE_RELOAD)
			return cost;
	foreach_ptr(ProllyTableMove, m, plan->tables)
		if (m->cost > SHARE_TABLE_USEC && list_member(shareable, makeString(m->name)))
			cost -= m->cost - SHARE_TABLE_USEC;
	return cost;
}

static char *
estimated_source(const ProllyNodeRef *target, Snapshot snap, List **emptied,
				 List **own_emptied)
{
	const ProllyMovePlannerRoutine *planner = prolly_move_planner_lookup("costs");
	SourceCandidates sc = {snap, false, NIL, NIL, NIL};
	ProllyMovePlan plan;
	ProllyMovePlan best;
	double		best_cost;
	char	   *source = NULL;
	List	   *shareable;

	/* (the session's branch first, then the others, in name order) */
	note_candidate(branch_ops()->current(), MyBranch, &sc);
	if (sc.names == NIL)
		return NULL;
	shareable = kept_shareable(target, snap, &sc.kept_seen);
	sc.others = true;
	if (branch_ops()->branches_while != NULL)
		branch_ops()->branches_while(note_candidate_while, &sc);
	else
		branch_ops()->branches(note_candidate, &sc);
	PROLLY_CALL(planner, plan, linitial(sc.roots), target, PROLLY_PLAN_RELOADS,
				INFINITY, &best);
	pgstat_count_prolly(PGSTAT_PROLLY_MOVE_PLANS, 1);
	reloaded_tables(&best, own_emptied);
	best_cost = cost_with_shares(&best, shareable);
	for (int i = 1; i < list_length(sc.names); i++)
	{
		/*
		 * (bounded by what it would have to cost less than, the best's less
		 * the margin; with tables to share, not bounded: one past it may
		 * cost less once they are shared)
		 */
		PlanOn		po = {planner, list_nth(sc.roots, i), target,
		shareable ? INFINITY : best_cost * SOURCE_COST_MARGIN, &plan};
		double		cost;

		branch_ops()->run_on(list_nth(sc.names, i), plan_here, &po);
		pgstat_count_prolly(PGSTAT_PROLLY_MOVE_PLANS, 1);
		cost = cost_with_shares(&plan, shareable);
		if (!plan.over && cost < best_cost * SOURCE_COST_MARGIN)
		{
			best = plan;
			best_cost = cost;
			source = list_nth(sc.names, i);
		}
	}
	if (source != NULL)
		reloaded_tables(&best, emptied);
	else
		*emptied = list_copy(*own_emptied);
	return source;
}

/*
 * Make the copies of branch name, whose head is commit, of the pages a
 * branch keeps as of the commit, if one does (#489): the session's, or
 * another.  Each table whose pages are still kept shares them, O(1), with
 * no lock on the source that holds up its writers; the rest are copied as
 * any first use copies them, those the move planner would reload or the
 * move drops (#713, #714) begun empty.  The baseline is the source's, with
 * each table made of kept pages taking its entry from the kept baseline,
 * as its change log is as it was then.  False, nothing made, if no branch
 * keeps the commit's pages (or the source has no baseline), or if the
 * source is another branch whose copied relations have a writer in the
 * middle of changing them (as for make()'s nowait).  This comes before
 * any source the planner would cost: the pages kept are the head's, with
 * nothing to move and no writer to wait for.
 */
static bool
make_of_kept(const char *name, const ProllyCommitInfo *commit)
{
	const ProllyMovePlannerRoutine *planner = prolly_move_planner_lookup("costs");
	List	   *emptied = NIL;
	ProllyDatabaseComponents dc;
	KeptSource	ks = {&commit->id, NULL};
	ProllyKeptFilter filter;
	const ProllyKeptCommit *kc;
	const char *source;
	bool		other;
	ProllyNodeRef thought;
	ProllyMovePlan plan;
	BranchKeptTable *tables;
	bool	   *used;
	List	   *names = NIL;
	ProllyNodeRef baseline;
	ProllyNodeRef from;
	ProllyNodeRef spliced;

	prolly_database_components(&dc);
	if (prolly_kept_commits <= 0 || branch_ops()->make_kept == NULL ||
		dc.canonicalizer->splice_baseline == NULL ||
		dc.matcache == NULL || !PROLLY_HAS(dc.matcache, visit))
		return false;
	filter.commit = &commit->id;
	filter.tables = NULL;
	filter.ntables = 0;
	PROLLY_CALL(dc.matcache, visit, GetActiveSnapshot(), &filter,
				find_kept_commit, &ks);
	if (ks.state == NULL)
		return false;
	kc = ks.state->kept;
	source = ks.state->name;
	if (kc->baseline.len == 0 || !branch_baseline(source, &baseline))
		return false;

	/*
	 * (the plan of the move from what the source is thought to be, for the
	 * tables not kept: those to begin empty; the source's alone looked up)
	 */
	other = strcmp(source, branch_ops()->current()) != 0;
	if (thought_root(source, ks.state->branch, GetActiveSnapshot(), &thought))
	{
		PlanOn		po = {planner, &thought, &commit->root, INFINITY, &plan};

		branch_ops()->run_on(source, plan_here, &po);
		pgstat_count_prolly(PGSTAT_PROLLY_MOVE_PLANS, 1);
		reloaded_tables(&plan, &emptied);
	}
	tables = palloc_array(BranchKeptTable, Max(kc->ntables, 1));
	used = palloc0_array(bool, Max(kc->ntables, 1));
	for (int t = 0; t < kc->ntables; t++)
	{
		tables[t].nrels = kc->tables[t].nrels;
		tables[t].rels = kc->tables[t].rels;
	}
	if (!OidIsValid(branch_ops()->make_kept(name, source, tables, kc->ntables,
											used, emptied, other)))
	{
		pgstat_count_prolly(PGSTAT_PROLLY_FIRST_USE_SOURCES_BUSY, 1);
		return false;
	}
	for (int t = 0; t < kc->ntables; t++)
	{
		if (used[t])
			names = lappend(names, pstrdup(kc->tables[t].name));
	}
	pgstat_count_prolly(PGSTAT_PROLLY_FIRST_USE_TABLES_KEPT, list_length(names));
	if (names != NIL)
	{
		ref_of(&kc->baseline, &from);
		PROLLY_CALL(dc.canonicalizer, splice_baseline, &baseline, &from, names,
					&spliced);
		baseline = spliced;
	}
	branch_set_baseline(name, &baseline, NULL);
	return true;
}

/*
 * Make the copies of a branch that has only its head: of the pages a
 * branch keeps as of its head (make_of_kept()), else a copy of the
 * nearest branch's to its head's root (#491), else of the branch the
 * move planner estimates is cheapest to move there, or else the
 * session's (each has the same changes since the same baseline as the
 * copy), moved to the root.
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
	List	   *emptied = NIL;
	List	   *own_emptied = NIL;
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
		mc.commit = &info;
		mc.source = NULL;
		if (!make_of_kept(name, &info))
		{
			foreach_ptr(NamedBranch, nb, root_candidates(ns.root))
				rank_source(nb->name, nb->branch, &ns);
			if (ns.rank == 0)
				ns.name = estimated_source(&info.root, ns.snap, &emptied,
										   &own_emptied);
			source = ns.name != NULL ? ns.name : branch_ops()->current();

			/*
			 * (another branch's copies only if none of its writers is in the
			 * middle of changing it: waiting for one, with the refs held, would
			 * hold up every commit of the session's branch for as long as that
			 * writer's transaction lasts; else the session's own, whose writers
			 * a first use waits for, as ever, with the tables its own plan
			 * empties)
			 */
			if (ns.name == NULL ||
				!OidIsValid(branch_ops()->make(name, source, emptied, true)))
			{
				if (ns.name != NULL)
					pgstat_count_prolly(PGSTAT_PROLLY_FIRST_USE_SOURCES_BUSY, 1);
				ns.name = NULL;
				ns.rank = 0;
				source = branch_ops()->current();
				branch_ops()->make(name, NULL, own_emptied, false);
			}
			branch_set_baseline(name,
								branch_baseline(source, &baseline) ? &baseline : NULL,
								NULL);
			mc.source = ns.rank == 3 ? &ns.known : NULL;
		}

		/*
		 * (moved to the head, unless the copies are there already: those of
		 * a branch whose working set is, if it still is)
		 */
		/* (the tables it makes are the relations the head's record names) */
		prolly_move_provenance = list_make1(&head);
		mc.head = head;
		PG_TRY(_provenance);
		{
			branch_ops()->run_on(name, move_copies_checked, &mc);
		}
		PG_FINALLY(_provenance);
		{
			prolly_move_provenance = NIL;
		}
		PG_END_TRY(_provenance);
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

/*
 * A first use's move (move_copies()): the copies are of the nearest
 * source's relations, whatever commit they are moved to, so a copy of
 * another relation than the one the head records under its name is
 * dropped first, and the move makes the head's relation; refused, the
 * error undoing it, if a table's copy still took a version of another
 * relation (prolly_refuse_mismatched_versions()).
 */
static void
move_copies_checked(void *arg)
{
	MoveCopies *mc = arg;
	ProllyNodeRef before;
	List	   *relids = NIL;
	bool		folded;

	/*
	 * (a copy of another relation than the head's of its name: the
	 * source's table made again since the branch was made, say.  Not the
	 * branch's table: dropped, so the move makes the head's relation)
	 */
	if (prolly_versions_may_mismatch(&mc->head, 1))
		prolly_drop_mismatched_tables(&mc->head, "use the branch", false);
	folded = prolly_versions_may_mismatch(&mc->head, 1) &&
		copies_working_set(NULL, &before);
	if (folded)
		relids = prolly_relations_of(&before);
	move_copies(arg);
	if (folded)
		prolly_refuse_mismatched_versions(&before, relids, &mc->head, 1,
										  PROLLY_MISMATCH_FIRST_USE);
}

static void
note_made(const char *name, Oid branch, void *arg)
{
	(void) hash_search((HTAB *) arg, name, HASH_ENTER, NULL);
}

/*
 * Visit the branches that have only a head (not yet copies); with prefix,
 * only those named with it, the refs' branch entries read from the first
 * such on (they are in name order), and each asked of the catalog alone.
 */
static void
visit_head_only(const char *prefix,
				void (*visit) (const char *name, void *arg), void *arg)
{
	ProllyNodeRef root;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	StringInfoData from;
	ProllyKey	start;
	List	   *names = NIL;
	ListCell   *lc;

	if (!refs_root(&root))
		return;
	initStringInfo(&from);
	appendStringInfoChar(&from, TAG_BRANCH);
	if (prefix != NULL)
		appendStringInfoString(&from, prefix);
	start.data = (const uint8 *) from.data;
	start.len = from.len;
	c = PROLLY_CALL(tree, cursor_open, &env, &root, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, &start, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		if (k.len < from.len || memcmp(k.data, from.data, from.len) != 0)
			break;
		names = lappend(names, pnstrdup((const char *) k.data + 1, k.len - 1));
	}
	PROLLY_CALL(tree, cursor_close, c);
	pfree(from.data);
	if (names == NIL)
		return;
	if (prefix != NULL)
	{
		foreach(lc, names)
		{
			if (!OidIsValid(branch_oid(lfirst(lc))))
				visit(lfirst(lc), arg);
		}
		return;
	}

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
 * Write a commit's tree of root and dependencies (what its versioned
 * objects need from outside); its id is the root's hash.  It is dated now,
 * as Dolt's are, not at the transaction's start, so a parent another
 * session committed since then is older than it; and never at or before a
 * parent's date (the clock may have stepped back, or a parent come from a
 * remote whose clock runs ahead), so along history the dates only rise.
 * (A parent dated outside timestamp's range, or at its last microsecond,
 * which only a foreign or corrupt commit could be, leaves the date as it
 * is.)  A date the commit was given (dolt_commit --date) is its date, as
 * Dolt's is.  Nothing is folded and no branch is moved.
 */
static void
copies_commit_root(const ProllyHash *root, const ProllyHash *dependencies,
				   const ProllyHash *parents, int nparents,
				   const char *author, const char *message,
				   const StringInfoData *identity, ProllyHash *id)
{
	TimestampTz at = GetCurrentTimestamp();
	int			nparents_dated = nparents;
	ProllyTreeBuilder *b;
	StringInfoData v;
	ProllyNodeRef ref;

	refs_init();

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
	b = PROLLY_CALL(tree, build_begin, &commit_env, CurrentMemoryContext);

	/*
	 * Keys in order: author, dependencies, height, identity (if any),
	 * message, parents, root, time.
	 */
	initStringInfo(&v);
	appendStringInfoString(&v, author);
	add_item(b, "author", &v);
	resetStringInfo(&v);
	put_hash(&v, dependencies);
	add_item(b, "dependencies", &v);
	resetStringInfo(&v);
	put_uint64(&v, (uint64) parents_height(parents, nparents));
	add_item(b, "height", &v);
	resetStringInfo(&v);
	if (identity && identity->len > 0)
		add_item(b, "identity", identity);
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
	prior_written_commit = last_written_commit;
	prior_written_root = last_written_root;
	last_written_commit = *id;
	last_written_root = *root;
	pfree(v.data);
	pgstat_count_prolly(PGSTAT_PROLLY_COMMITS, 1);
}

/*
 * A commit of root whose dependencies are folded now, from the catalogs as
 * the transaction sees them (copies_commit_root()).
 */
static void
write_commit(const ProllyHash *root, const ProllyHash *parents, int nparents,
			 const char *author, const char *message,
			 const StringInfoData *identity, ProllyHash *id)
{
	ProllyNodeRef deps;
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, fold_dependencies, &deps);
	copies_commit_root(root, &deps.hash, parents, nparents, author, message,
					   identity, id);
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

/* The most items a commit has (write_commit's). */
#define COMMIT_ITEMS	8

/* Is a commit's item k the one named name? */
static bool
item_is(const ProllyKey *k, const char *name)
{
	return k->len == strlen(name) && memcmp(k->data, name, k->len) == 0;
}

/* Does item k sort after the one named name (by bytes, as a commit's do)? */
static bool
item_after(const ProllyKey *k, const char *name)
{
	size_t		len = strlen(name);
	int			c = memcmp(k->data, name, Min(k->len, len));

	return c > 0 || (c == 0 && k->len > len);
}

/*
 * A commit's items are read in one pass: a get of each would descend the
 * commit's tree once per item.  Any hash may name a commit here (a ref
 * given in hex), so the pass stops past the last of a commit's items, or
 * past as many items as a commit has: a descent of whatever tree the hash
 * names, and a leaf or so.
 */
static bool
copies_commit_get(const ProllyHash *id, ProllyCommitInfo *info)
{
	ProllyNodeRef ref;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	bool		has_root = false;
	int			nitems = 0;

	refs_init();
	if (id->len == 0)
		return false;
	if (!PROLLY_CALL(env.chunk_store, has, env.chunks, id))
	{
		/* (one past the history horizon: said so, with how to repair it) */
		prolly_window_check_missing(id);
		return false;
	}
	ref_of(id, &ref);
	memset(info, 0, sizeof(*info));
	info->id = *id;
	info->author = pstrdup("");
	info->message = pstrdup("");
	c = PROLLY_CALL(tree, cursor_open, &env, &ref, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		const char *p = (const char *) v.data;
		const char *end = p + v.len;

		/*
		 * (past a commit's last item in key order, "time", or past as many
		 * items as a commit has: whatever else the hash names, no more of
		 * it is read)
		 */
		if (item_after(&k, "time") || ++nitems > COMMIT_ITEMS)
		{
			if (nitems > COMMIT_ITEMS)
				has_root = false;
			break;
		}
		if (item_is(&k, "root"))
		{
			ProllyHash	root;

			(void) get_hash(p, end, &root);
			ref_of(&root, &info->root);
			has_root = true;
		}
		else if (item_is(&k, "parents"))
		{
			while (p < end && info->nparents < PROLLY_MAX_PARENTS)
				p = get_hash(p, end, &info->parents[info->nparents++]);
		}
		else if (item_is(&k, "author"))
			info->author = pnstrdup(p, v.len);
		else if (item_is(&k, "message"))
			info->message = pnstrdup(p, v.len);
		else if (item_is(&k, "dependencies"))
		{
			ProllyHash	deps;

			(void) get_hash(p, end, &deps);
			ref_of(&deps, &info->dependencies);
			info->has_dependencies = true;
		}
		else if (item_is(&k, "height") || item_is(&k, "time"))
		{
			StringInfoData n = {unconstify(char *, p), v.len, v.len, 0};
			uint64		height;

			if (item_is(&k, "time"))
				(void) get_time(&n, &info->committed_at);
			else if (get_uint64(&n, &height) && height > 0 &&
					 height <= PG_INT64_MAX)
			{
				info->height = (int64) height;
				info->has_height = true;
			}
		}
		else if (item_is(&k, "identity") && v.len > 0)
		{
			info->identity = pnstrdup(p, v.len);
			info->identity_len = v.len;
		}
	}
	PROLLY_CALL(tree, cursor_close, c);
	if (!has_root)
		return false;			/* a chunk, but not a commit */
	pgstat_count_prolly(PGSTAT_PROLLY_COMMITS_READ, 1);
	return true;
}

/* The database's commit cache, if its configuration has one. */
static const ProllyCommitCacheRoutine *
commit_cache(void)
{
	static const ProllyCommitCacheRoutine *cc = NULL;
	static bool looked = false;

	if (!looked)
	{
		ProllyDatabaseComponents dc;

		prolly_database_components(&dc);
		cc = dc.commit_cache;
		looked = true;
	}
	return cc;
}

/*
 * What commit id links to (its root, parents and height): from the commit
 * cache, if it has them for the store's GC generation now, else read
 * (copies_commit_get()) and remembered there.  False if id names no commit
 * in the store.  Walking history this way reads each commit once for every
 * backend, rather than once each time the node cache has let go of it.
 */
static bool
commit_links(const ProllyHash *id, ProllyCommitLinks *links)
{
	const ProllyCommitCacheRoutine *cc = commit_cache();
	bool		cached;
	uint32		generation = 0;
	ProllyCommitInfo info;

	refs_init();
	cached = cc != NULL && PROLLY_HAS(env.chunk_store, generation);
	if (cached)
	{
		generation = PROLLY_CALL(env.chunk_store, generation, env.chunks);
		if (PROLLY_CALL(cc, lookup, MyDatabaseId, generation, id, links))
			return true;
	}
	if (!copies_commit_get(id, &info))
		return false;
	memset(links, 0, sizeof(*links));
	links->root = info.root.hash;
	links->nparents = info.nparents;
	for (int i = 0; i < info.nparents; i++)
		links->parents[i] = info.parents[i];
	links->height = info.has_height ? info.height : 0;
	if (cached)
		PROLLY_CALL(cc, remember, MyDatabaseId, generation, id, links);
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
	ProllyCommitLinks links;
	bool		branch_only = false;

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
	{
		refname += 11;
		branch_only = true;
	}
	if (branch_head(refname, commit_id) || OidIsValid(branch_oid(refname)))
		return commit_id->len > 0;
	if (branch_only)
		return false;
	/* a tag, by its name, or its full ref name (only a tag's) */
	if (strncmp(refname, "refs/tags/", 10) == 0 && refname[10] != '\0' &&
		!tag_entry(refname, NULL, NULL))
		return tag_entry(refname + 10, NULL, commit_id);
	if (tag_entry(refname, NULL, commit_id))
		return true;
	if (tracking_head(refname, commit_id))
		return true;
	if (!parse_commit_id(refname, commit_id))
		return false;
	if (commit_links(commit_id, &links))
		return true;
	/* (a commit named by its id, not here: past the horizon, maybe) */
	prolly_window_check_absent(commit_id);
	return false;
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
		ProllyCommitLinks links;

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
				found = commit_links(commit_id, &links) && links.nparents > 0;
				if (found)
					*commit_id = links.parents[0];
			}
		}
		else if (op == '^')
		{
			if (n == 0)
				continue;
			found = commit_links(commit_id, &links) && n <= links.nparents;
			if (found)
				*commit_id = links.parents[n - 1];
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
	/* (not a fold whose trees only this backend has) */
	if (fold_caches_usable() && !prolly_local_chunks_active())
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
 * A name in a relation's line: each backslash in it written as two, each
 * newline as a backslash and 'n' and, where it is followed by another
 * field (spaces), each space as a backslash and 's', so that no name,
 * whatever it holds, makes a line or a field of its own.
 */
static void
line_name(StringInfo out, const char *name, bool spaces)
{
	for (const char *c = name; *c; c++)
	{
		if (*c == '\\')
			appendStringInfoString(out, "\\\\");
		else if (*c == '\n')
			appendStringInfoString(out, "\\n");
		else if (*c == ' ' && spaces)
			appendStringInfoString(out, "\\s");
		else
			appendStringInfoChar(out, *c);
	}
}

/*
 * A relation's line in a record of tables: its OID (10 digits), a space,
 * its owner's OID (10 digits), a space, its kind (relkind), a space, its
 * owner's name (line_name(), spaces escaped), a space, and its qualified
 * name (line_name()); then a newline.  The owner's name is kept with its
 * OID so that a role made later with the OID, once the counter wraps, is
 * told from the one that owned the relation.
 */
static void
relation_line(StringInfo out, Oid relid, Oid owner, char relkind,
			  const char *name)
{
	char	   *owner_name = GetUserNameFromId(owner, true);

	appendStringInfo(out, "%010u %010u %c ", relid, owner, relkind);
	line_name(out, owner_name ? owner_name : "", true);
	appendStringInfoChar(out, ' ');
	line_name(out, name, false);
	appendStringInfoChar(out, '\n');
}

/*
 * A name of a relation's line (line_name()'s), from *c on to the end of
 * the field (a space, if spaces, else the line's end), unescaped into out;
 * false if malformed.  *c is left at the field's end.
 */
static bool
read_line_name(const char **c, StringInfo out, bool spaces)
{
	const char *p = *c;

	for (; *p && *p != '\n' && !(spaces && *p == ' '); p++)
	{
		if (*p != '\\')
			appendStringInfoChar(out, *p);
		else if (p[1] == '\\')
			appendStringInfoChar(out, *++p);
		else if (p[1] == 'n')
		{
			appendStringInfoChar(out, '\n');
			p++;
		}
		else if (p[1] == 's' && spaces)
		{
			appendStringInfoChar(out, ' ');
			p++;
		}
		else
			return false;
	}
	*c = p;
	return true;
}

/*
 * The relations a record of tables lists (relation_line()'s lines, from
 * data on), each a ProllyCommitRelation: a line of any other form is
 * refused, as a damaged record, never read in part.
 */
typedef enum RelationLines
{
	LINES_OWNED,				/* a record, with its routines and types */
	LINES_RELATIONS,			/* a record, without them */
	LINES_ROUTINES				/* the routines and types a record names */
} RelationLines;

static List *relation_lines_of(const char *data, RelationLines what);

static List *
relation_lines(const char *data)
{
	return relation_lines_of(data, LINES_OWNED);
}

/*
 * ... with or without the lines of the routines and types its last line
 * may name, by their hash (table_names()).
 */
static List *
relation_lines_of(const char *data, RelationLines what)
{
	List	   *out = NIL;

	for (const char *p = data; *p;)
	{
		ProllyCommitRelation *r;
		StringInfoData owner;
		StringInfoData name;
		const char *c;

		if (*p == '@')
		{
			const char *nl = strchr(p, '\n');
			StringInfoData key;
			StringInfoData v;

			if (what == LINES_ROUTINES || nl == NULL ||
				nl - p != 1 + 2 * PG_SHA256_DIGEST_LENGTH || nl[1] != '\0')
				goto bad;
			for (const char *h = p + 1; h < nl; h++)
				if (!isxdigit((unsigned char) *h))
					goto bad;
			if (what == LINES_RELATIONS)
				break;
			initStringInfo(&key);
			initStringInfo(&v);
			appendStringInfoChar(&key, TAG_OWNED);
			appendBinaryStringInfo(&key, p + 1, nl - p - 1);
			if (!refs_get(key.data, key.len, &v))
				goto bad;
			out = list_concat(out, relation_lines_of(v.data, LINES_ROUTINES));
			pfree(key.data);
			pfree(v.data);
			break;
		}
		r = palloc(sizeof(ProllyCommitRelation));

		for (int i = 0; i < 10; i++)
			if (!isdigit((unsigned char) p[i]))
				goto bad;
		if (p[10] != ' ')
			goto bad;
		r->relid = (Oid) strtoul(p, NULL, 10);
		for (int i = 11; i < 21; i++)
			if (!isdigit((unsigned char) p[i]))
				goto bad;
		if (p[21] != ' ')
			goto bad;
		r->owner = (Oid) strtoul(p + 11, NULL, 10);
		r->relkind = p[22];
		if (strchr("rpmvSfPTNC", r->relkind) == NULL || r->relkind == '\0' ||
			p[23] != ' ')
			goto bad;
		c = p + 24;
		initStringInfo(&owner);
		if (!read_line_name(&c, &owner, true) || *c != ' ')
			goto bad;
		c++;
		initStringInfo(&name);
		if (!read_line_name(&c, &name, false) || *c != '\n' || name.len == 0)
			goto bad;
		r->owner_name = owner.data;
		r->name = name.data;
		out = lappend(out, r);
		p = c + 1;
	}
	return out;

bad:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("prolly refs hold a damaged record of relations")));
	return NIL;
}

/*
 * The hash a record (value, len bytes) names its routines and types by,
 * in its last line ('@' and the hash in hex), into hash: false if it names
 * none.
 */
static bool
owned_reference(const char *value, int len, char *hash)
{
	const int	hlen = 2 * PG_SHA256_DIGEST_LENGTH;
	const char *at = value + len - hlen - 2;

	if (len < hlen + 2 || at[0] != '@' || value[len - 1] != '\n' ||
		(at > value && at[-1] != '\n'))
		return false;
	for (int i = 1; i <= hlen; i++)
		if (!isxdigit((unsigned char) at[i]))
			return false;
	memcpy(hash, at + 1, hlen);
	hash[hlen] = '\0';
	return true;
}

/*
 * A relation's qualified name as the records of relations write it, and
 * as commits name tables: quote_qualified_identifier()'s with
 * quote_all_identifiers off, whatever the session's setting, so a record
 * written in one session matches a name looked up in another.
 */
char *
prolly_record_relation_name(const char *nspname, const char *relname)
{
	bool		save = quote_all_identifiers;
	char	   *name;

	quote_all_identifiers = false;
	PG_TRY();
	{
		name = pstrdup(quote_qualified_identifier(nspname, relname));
	}
	PG_FINALLY();
	{
		quote_all_identifiers = save;
	}
	PG_END_TRY();
	return name;
}

/* A line of table_names() for another object (prolly_owned_objects()) */
static void
owned_object_line(Oid oid, Oid owner, char kind, const char *name, void *arg)
{
	List	  **lines = arg;
	StringInfoData line;

	initStringInfo(&line);
	relation_line(&line, oid, owner, kind, name);
	*lines = lappend(*lines, line.data);
}

/*
 * The session's prolly tables, partitioned tables and materialized views,
 * each a relation_line(), by relid: as the branch's catalog names them now;
 * and each other object a commit may hold (prolly_owned_objects()), so that
 * a move making one from a commit made of them finds its owner.
 */
/*
 * The branch's routines and types, as a record of relations lists them:
 * their lines (relation_line()'s, sorted) are stored apart, once, under
 * their content's hash (TAG_OWNED), and the record names them by it, in a
 * last line of '@' and the hash in hex.  A commit whose routines and types
 * are those its parent's record named (as most are) names the same lines,
 * which are neither written nor worked out again: the canonicalizer keeps
 * its listing from one commit to the next (prolly_owned_routines()), and
 * the session the lines and hash it made of it, until the listing changes
 * or a role is renamed (whose name a line holds).  A commit that changed
 * one routine writes a new entry, but not the whole listing again: a long
 * value is held out of line in a value tree cut by its content, so the new
 * one shares every chunk but those around the changed line with the last
 * (the lines are sorted by OID, a new routine's last).
 */
static uint64 owned_generation = 0;
static char *owned_hash = NULL;	/* (NULL: none listed) */
static char *owned_text = NULL;
static bool owned_callback = false;

static void
owned_role_renamed(Datum arg, int cacheid, uint32 hashvalue)
{
	owned_generation = 0;
}

static void
owned_names(StringInfo out)
{
	List	   *lines = NIL;
	StringInfoData key;
	StringInfoData v;
	uint64		generation = owned_generation;

	if (!owned_callback)
	{
		CacheRegisterSyscacheCallback(AUTHOID, owned_role_renamed, (Datum) 0);
		owned_callback = true;
	}
	if (prolly_owned_routines(&generation, owned_object_line, &lines))
	{
		StringInfoData text;
		pg_cryptohash_ctx *ctx;
		uint8		digest[PG_SHA256_DIGEST_LENGTH];

		initStringInfo(&text);
		list_sort(lines, line_cmp);
		foreach_ptr(char, line, lines)
			appendStringInfoString(&text, line);
		owned_generation = 0;
		if (owned_text)
			pfree(owned_text);
		if (owned_hash)
			pfree(owned_hash);
		owned_text = MemoryContextStrdup(TopMemoryContext, text.data);
		owned_hash = NULL;
		if (text.len > 0)
		{
			ctx = pg_cryptohash_create(PG_SHA256);
			if (ctx == NULL || pg_cryptohash_init(ctx) < 0 ||
				pg_cryptohash_update(ctx, (const uint8 *) text.data, text.len) < 0 ||
				pg_cryptohash_final(ctx, digest, sizeof(digest)) < 0)
				elog(ERROR, "could not hash the routines and types of a record of relations");
			pg_cryptohash_free(ctx);
			owned_hash = MemoryContextAllocZero(TopMemoryContext,
												2 * PG_SHA256_DIGEST_LENGTH + 1);
			hex_encode((const char *) digest, sizeof(digest), owned_hash);
		}
		pfree(text.data);
		/* (known only once its lines and hash are) */
		owned_generation = generation;
	}
	if (owned_hash == NULL)
		return;
	/* (stored once: a transaction that aborted may have stored it alone) */
	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_OWNED);
	appendStringInfoString(&key, owned_hash);
	if (!refs_get(key.data, key.len, &v))
	{
		resetStringInfo(&v);
		appendStringInfoString(&v, owned_text);
		refs_put(key.data, key.len, &v);
	}
	pfree(key.data);
	pfree(v.data);
	appendStringInfo(out, "@%s\n", owned_hash);
}

static void
table_names(StringInfo out)
{
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	SysScanDesc scan = catalog_branch_beginscan(rel, MyBranch);
	HeapTuple	tup;
	List	   *lines = NIL;
	ListCell   *lc;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if ((c->relam == PROLLY_TABLE_AM_OID &&
			 (c->relkind == RELKIND_RELATION || c->relkind == RELKIND_MATVIEW)) ||
			c->relkind == RELKIND_PARTITIONED_TABLE)
		{
			StringInfoData line;

			initStringInfo(&line);
			relation_line(&line, c->oid, c->relowner, c->relkind,
						  prolly_record_relation_name(get_namespace_name(c->relnamespace),
													  NameStr(c->relname)));
			lines = lappend(lines, line.data);
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	prolly_owned_objects(owned_object_line, &lines);
	list_sort(lines, line_cmp);
	foreach(lc, lines)
		appendStringInfoString(out, lfirst(lc));
	owned_names(out);
}

/*
 * The session's relations with privileges of their own other than its
 * prolly and partitioned tables (views, materialized views, sequences and
 * foreign tables), each a relation_line(), by relid: as table_names().
 */
static void
privileged_names(StringInfo out)
{
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	SysScanDesc scan = catalog_branch_beginscan(rel, MyBranch);
	HeapTuple	tup;
	List	   *lines = NIL;
	ListCell   *lc;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relkind == RELKIND_VIEW || c->relkind == RELKIND_MATVIEW ||
			c->relkind == RELKIND_SEQUENCE ||
			c->relkind == RELKIND_FOREIGN_TABLE)
		{
			StringInfoData line;

			initStringInfo(&line);
			relation_line(&line, c->oid, c->relowner, c->relkind,
						  prolly_record_relation_name(get_namespace_name(c->relnamespace),
													  NameStr(c->relname)));
			lines = lappend(lines, line.data);
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	list_sort(lines, line_cmp);
	foreach(lc, lines)
		appendStringInfoString(out, lfirst(lc));
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

/*
 * While set, the tables (qualified names) a commit being made changed in
 * the working set before the move it commits (a merge, pick or revert over
 * uncommitted changes, set aside or left out): the commit holds them as its
 * first parent did, so each is recorded made only from a relation of the
 * class its first parent's record lists.
 */
List	   *prolly_commit_local_tables = NIL;

/*
 * The relations a commit was made from: in the records' tree, under the
 * commit's id (its hash's bytes), a line for each of the session's prolly
 * tables, as table_names() lists them.  Every commit this database makes of
 * a branch records them, so a history read can tell a version of the
 * session's table from another table's of its name, which the commit's
 * names and identities can't (a table made again keeps its name's
 * identity).  The tree's root is the refs' TAG_RECORDS entry: the records
 * grow with every commit, so kept apart from the refs, whose few entries a
 * commit changes stay as small, and as cheap to change, however many
 * commits there are; a commit adds its record to the records' tree, one
 * path of it, and changes the one refs entry naming its root.
 */
static void
empty_records(ProllyNodeRef *records)
{
	ProllyTreeBuilder *b = PROLLY_CALL(tree, build_begin, &records_env,
									   CurrentMemoryContext);

	PROLLY_CALL(tree, build_finish, b, records);
}

/* The records' tree of refs root refs (*any false: it has none yet). */
static void
records_under(const ProllyNodeRef *refs, ProllyNodeRef *records, bool *any)
{
	char		tag = TAG_RECORDS;
	ProllyKey	k = {(const uint8 *) &tag, 1};
	StringInfoData v;

	memset(records, 0, sizeof(*records));
	*any = false;
	initStringInfo(&v);
	if (refs->hash.len > 0 && PROLLY_CALL(tree, get, &env, refs, &k, &v))
	{
		ProllyHash	h;

		(void) get_hash(v.data, v.data + v.len, &h);
		ref_of(&h, records);
		*any = true;
	}
	pfree(v.data);
}

/* ... as the transaction sees them, its changes not yet applied too */
static bool
records_now(ProllyNodeRef *records)
{
	char		tag = TAG_RECORDS;
	StringInfoData v;
	bool		any;

	memset(records, 0, sizeof(*records));
	initStringInfo(&v);
	any = refs_get(&tag, 1, &v);
	if (any)
	{
		ProllyHash	h;

		(void) get_hash(v.data, v.data + v.len, &h);
		ref_of(&h, records);
	}
	pfree(v.data);
	return any;
}

/* commit's record, into value (false: unrecorded) */
static bool
record_of_commit(const ProllyHash *commit, StringInfo value)
{
	ProllyNodeRef records;
	ProllyKey	k = {commit->bytes, commit->len};

	return records_now(&records) &&
		PROLLY_CALL(tree, get, &records_env, &records, &k, value);
}

/* Record commit as made from the tables lines lists (table_names()'s). */
static void
note_relations(const ProllyHash *commit, const char *lines)
{
	char		tag = TAG_RECORDS;
	ProllyNodeRef records;
	ProllyNodeRef next;
	ProllyEdit	e;
	StringInfoData v;

	if (!records_now(&records))
		empty_records(&records);
	e.key.data = commit->bytes;
	e.key.len = commit->len;
	e.is_delete = false;
	e.value.data = (const uint8 *) lines;
	e.value.len = strlen(lines);
	PROLLY_CALL(tree, apply, &records_env, &records, &e, 1, &next);
	initStringInfo(&v);
	put_hash(&v, &next.hash);
	refs_put(&tag, 1, &v);
	pfree(v.data);
	if (scoped)
	{
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
		ProllyHash *c = palloc(sizeof(ProllyHash));

		*c = *commit;
		scoped_records = lappend(scoped_records, c);
		MemoryContextSwitchTo(old);
	}
}

/*
 * The relations commit was made from (ProllyCommitRelation *); false if
 * unrecorded.
 */
static bool
copies_commit_relations(const ProllyHash *commit, List **relations)
{
	StringInfoData v;
	bool		found;

	refs_init();
	initStringInfo(&v);
	found = record_of_commit(commit, &v);
	if (found)
		*relations = relation_lines(v.data);
	pfree(v.data);
	return found;
}

/*
 * Does the store's root hold refs of this build's format (into *root)?
 * Asked without refusing: a store given another root (a test's chunk, an
 * older build's refs) has nothing a step after its collection may forget.
 */
static bool
refs_tree_held(ProllyNodeRef *root)
{
	ProllyHash	h;
	StringInfoData data;
	StringInfoData v;
	ProllyChunkNode chunk;
	ProllyKey	k = {(const uint8 *) "!", 1};
	bool		held;

	flush_scoped();
	PROLLY_CALL(env.chunk_store, root, env.chunks, &h);
	if (scoped)
	{
		rebase_scoped(&h);
		h = scoped_mine;
	}
	if (h.len == 0)
		return false;
	initStringInfo(&data);
	if (!PROLLY_CALL(env.chunk_store, get, env.chunks, &h, &data) ||
		!PROLLY_CALL(env.chunk_format, open, (const uint8 *) data.data,
					 data.len, &chunk))
	{
		pfree(data.data);
		return false;
	}
	pfree(data.data);
	ref_of(&h, root);
	initStringInfo(&v);
	held = PROLLY_CALL(tree, get, &env, root, &k, &v) &&
		v.len == strlen(REFS_FORMAT) && memcmp(v.data, REFS_FORMAT, v.len) == 0;
	pfree(v.data);
	return held;
}

/*
 * The commits (ProllyHash *) whose records the records' tree records holds
 * that the store doesn't have, in key order.
 */
static List *
records_gone(const ProllyNodeRef *records)
{
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	List	   *gone = NIL;

	c = PROLLY_CALL(tree, cursor_open, &records_env, records, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		ProllyHash *commit = palloc0(sizeof(ProllyHash));

		if (k.len > PROLLY_HASH_MAX_LEN)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly refs hold a damaged record of relations")));
		commit->len = k.len;
		memcpy(commit->bytes, k.data, k.len);
		if (!PROLLY_CALL(env.chunk_store, has, env.chunks, commit))
			gone = lappend(gone, commit);
		else
			pfree(commit);
	}
	PROLLY_CALL(tree, cursor_close, c);
	return gone;
}

/* A set of strings (keys of keysize bytes, NUL-padded), in the caller's context */
static HTAB *
string_set(const char *what, int keysize)
{
	HASHCTL		ctl;

	ctl.keysize = keysize;
	ctl.entrysize = keysize;
	ctl.hcxt = CurrentMemoryContext;
	return hash_create(what, 256, &ctl, HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
}

/*
 * The keys of the routines' and types' entries (TAG_OWNED) under root that
 * no record (a commit's, in the records' tree records; a branch head's,
 * TAG_NAMES) names, but through those of the commits gone, in key order.
 */
static List *
owned_unreached(const ProllyNodeRef *root, const ProllyNodeRef *records,
				List *gone)
{
	const int	hlen = 2 * PG_SHA256_DIGEST_LENGTH;
	HTAB	   *reached = string_set("prolly owned reached", hlen + 1);
	HTAB	   *forgetting;
	HASHCTL		ctl;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	char		tag = TAG_NAMES;
	ProllyKey	at = {(const uint8 *) &tag, 1};
	char		otag = TAG_OWNED;
	ProllyKey	from = {(const uint8 *) &otag, 1};
	char		hash[2 * PG_SHA256_DIGEST_LENGTH + 1];
	List	   *out = NIL;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	forgetting = hash_create("prolly records forgotten", 64, &ctl,
							 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	foreach_ptr(ProllyHash, commit, gone)
		(void) hash_search(forgetting, commit, HASH_ENTER, NULL);
	c = PROLLY_CALL(tree, cursor_open, &records_env, records, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		ProllyHash	commit;

		memset(&commit, 0, sizeof(commit));
		commit.len = Min(k.len, PROLLY_HASH_MAX_LEN);
		memcpy(commit.bytes, k.data, commit.len);
		if (hash_search(forgetting, &commit, HASH_FIND, NULL) == NULL &&
			owned_reference((const char *) v.data, v.len, hash))
			(void) hash_search(reached, hash, HASH_ENTER, NULL);
	}
	PROLLY_CALL(tree, cursor_close, c);
	c = PROLLY_CALL(tree, cursor_open, &env, root, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, &at, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v) &&
		   k.len >= 1 && k.data[0] == TAG_NAMES)
		if (owned_reference((const char *) v.data, v.len, hash))
			(void) hash_search(reached, hash, HASH_ENTER, NULL);
	PROLLY_CALL(tree, cursor_close, c);
	c = PROLLY_CALL(tree, cursor_open, &env, root, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, &from, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v) &&
		   k.len >= 1 && k.data[0] == TAG_OWNED)
	{
		char	   *h = pnstrdup((const char *) k.data + 1, k.len - 1);

		if (k.len - 1 != hlen || hash_search(reached, h, HASH_FIND, NULL) == NULL)
			out = lappend(out, pnstrdup((const char *) k.data, k.len));
		pfree(h);
	}
	PROLLY_CALL(tree, cursor_close, c);
	hash_destroy(forgetting);
	hash_destroy(reached);
	return out;
}

/*
 * Forget the relation records of commits the store no longer has (a
 * collection took them): a record is kept by its commit's key alone, never
 * as a reference a collection follows, so it would outlive its commit.
 * Asked of the store's index, after the collection: a collected commit's
 * chunk isn't read.  And forget the routines' and types' entries no record
 * left names (owned_unreached()): records share them, so each goes with the
 * last record naming it, or alone, stored by a transaction that aborted.
 * If any is found, the refs are held whole and both asked again of the
 * root then, so that a commit made again meanwhile (the same id) by a
 * transaction that published since keeps its record, and one publishing
 * after waits for this one and puts its record back over it, with the
 * entries it names (records_rebase()); all are changed in one edit of the
 * records' tree and one of the refs.  True if any was forgotten.
 */
static bool
copies_forget_records(void)
{
	ProllyNodeRef root;
	ProllyNodeRef new_root;
	ProllyNodeRef records;
	List	   *gone;
	List	   *unreached;
	ProllyEdit *edits;
	int			n = 0;
	bool		any;
	char		tag = TAG_RECORDS;
	StringInfoData rv;

	refs_init();
	if (!refs_tree_held(&root))
		return false;
	records_under(&root, &records, &any);
	if (!any)
		return false;
	gone = records_gone(&records);
	if (gone == NIL && owned_unreached(&root, &records, NIL) == NIL)
		return false;
	lock_refs();
	if (!refs_root(&root))
		return false;
	records_under(&root, &records, &any);
	if (!any)
		return false;
	gone = records_gone(&records);
	unreached = owned_unreached(&root, &records, gone);
	INJECTION_POINT("prolly-forget-records-locked", NULL);
	if (gone == NIL && unreached == NIL)
		return false;
	/* (TAG_RECORDS's entry first, in key order, then TAG_OWNED's) */
	edits = palloc0(sizeof(ProllyEdit) * (1 + list_length(unreached)));
	initStringInfo(&rv);
	if (gone != NIL)
	{
		ProllyEdit *re = palloc0(sizeof(ProllyEdit) * list_length(gone));
		ProllyNodeRef next;
		int			m = 0;

		foreach_ptr(ProllyHash, commit, gone)
		{
			re[m].key.data = commit->bytes;
			re[m].key.len = commit->len;
			re[m].is_delete = true;
			m++;
		}
		PROLLY_CALL(tree, apply, &records_env, &records, re, m, &next);
		put_hash(&rv, &next.hash);
		edits[n].key.data = (const uint8 *) &tag;
		edits[n].key.len = 1;
		edits[n].is_delete = false;
		edits[n].value.data = (const uint8 *) rv.data;
		edits[n].value.len = rv.len;
		n++;
	}
	foreach_ptr(char, key, unreached)
	{
		edits[n].key.data = (const uint8 *) key;
		edits[n].key.len = strlen(key);
		edits[n].is_delete = true;
		n++;
	}
	PROLLY_CALL(tree, apply, &env, &root, edits, n, &new_root);
	if (!PROLLY_CALL(env.chunk_store, move_root, env.chunks, &root.hash,
					 &new_root.hash))
		elog(ERROR, "prolly refs moved while held");
	pgstat_count_prolly(PGSTAT_PROLLY_RECORDS_FORGOTTEN, list_length(gone));
	return true;
}

/*
 * Of the first parent's relation record (parent, or its lines had), the
 * lines of relations the session still has (of now's): a commit that
 * isn't its working set (a merge's, a pick's or a revert's, made over
 * uncommitted changes) holds a table as the parent's relation made it, or
 * one made by the move, never the working set's own, so it is recorded
 * made only from what both hold.
 */
static char *
relations_kept(const ProllyHash *parent, const char *had, const char *now,
			   List *local)
{
	StringInfoData v;
	StringInfoData out;
	bool		have;
	List	   *before = NIL;
	ListCell   *lc;

	initStringInfo(&v);
	initStringInfo(&out);
	if (had != NULL)
		appendStringInfoString(&v, had);
	have = had != NULL || record_of_commit(parent, &v);
	if (have)
		before = relation_lines(v.data);
	foreach(lc, relation_lines(now))
	{
		ProllyCommitRelation *r = lfirst(lc);
		bool		keep = false;
		bool		changed = false;
		ListCell   *nc;

		/*
		 * (a table the commit holds as the working set has it: one the
		 * working set didn't change, when the uncommitted changes are named)
		 */
		foreach(nc, local)
			if (strcmp(lfirst(nc), r->name) == 0)
				changed = true;
		if (local != NIL && !changed)
			keep = true;

		/* (else as the parent had it: the same relation) */
		else if (have)
		{
			ListCell   *bc;

			foreach(bc, before)
				if (((ProllyCommitRelation *) lfirst(bc))->relid == r->relid)
					keep = true;
		}
		if (keep)
			relation_line(&out, r->relid, r->owner, r->relkind, r->name);
	}
	pfree(v.data);
	return out.data;
}

static void
privileged_key(StringInfo key, const char *name)
{
	appendStringInfoChar(key, TAG_PRIVILEGED);
	appendStringInfoString(key, name);
}

/*
 * The branch's record of its head's other relations with privileges
 * (privileged_names()), as note_head_names() records its tables: of the
 * working set, or (partial: the head isn't the working set folded) only
 * those its first parent's record has too, of the same OID and name (one
 * made again since, uncommitted, isn't the commit's).
 */
static void
note_head_privileged(const char *name, const ProllyHash *head,
					 const ProllyHash *partial)
{
	StringInfoData key;
	StringInfoData v;
	StringInfoData now;
	char	   *hex = prolly_hash_hex(head);
	char	   *lines;

	initStringInfo(&key);
	initStringInfo(&v);
	initStringInfo(&now);
	privileged_key(&key, name);
	privileged_names(&now);
	lines = now.data;
	if (partial != NULL)
	{
		char	   *phex = prolly_hash_hex(partial);
		List	   *before = NIL;
		StringInfoData kept;

		if (refs_get(key.data, key.len, &v) &&
			strncmp(v.data, phex, strlen(phex)) == 0 && v.data[strlen(phex)] == '\n')
			before = relation_lines(v.data + strlen(phex) + 1);
		initStringInfo(&kept);
		foreach_ptr(ProllyCommitRelation, r, relation_lines(now.data))
			foreach_ptr(ProllyCommitRelation, b, before)
				if (b->relid == r->relid && strcmp(b->name, r->name) == 0)
				{
					relation_line(&kept, r->relid, r->owner, r->relkind, r->name);
					break;
				}
		lines = kept.data;
		pfree(phex);
	}
	/*
	 * (none, none written: a record of an earlier head, left, is of no
	 * head this branch has, so lists nothing of it, as wanted)
	 */
	if (lines[0] != '\0')
	{
		resetStringInfo(&v);
		appendStringInfo(&v, "%s\n%s", hex, lines);
		refs_put(key.data, key.len, &v);
	}
	pfree(hex);
	pfree(key.data);
	pfree(v.data);
}

/*
 * The branch's record of its head's tables, and the head's relation
 * record (the same lines): of the working set, or (partial: the head isn't
 * the working set folded) only of what its first parent's record and the
 * working set both hold (relations_kept()): the parent's taken from the
 * branch's record, when that is of it, which the record's put reads anyway.
 */
static void
note_head_names(const char *name, const ProllyHash *head,
				const ProllyHash *partial, const ProllyHash *first_parent)
{
	StringInfoData key;
	StringInfoData v;
	char	   *hex = prolly_hash_hex(head);
	char	   *lines;

	initStringInfo(&key);
	initStringInfo(&v);
	names_key(&key, name);
	if (partial == NULL && prolly_commit_local_tables != NIL && first_parent != NULL)
		partial = first_parent;
	if (partial != NULL)
	{
		char	   *phex = prolly_hash_hex(partial);
		char	   *had = NULL;
		StringInfoData now;

		if (refs_get(key.data, key.len, &v) &&
			strncmp(v.data, phex, strlen(phex)) == 0 && v.data[strlen(phex)] == '\n')
			had = pstrdup(v.data + strlen(phex) + 1);
		initStringInfo(&now);
		table_names(&now);
		lines = relations_kept(partial, had, now.data, prolly_commit_local_tables);
		pfree(phex);
	}
	else
	{
		StringInfoData now;

		initStringInfo(&now);
		table_names(&now);
		lines = now.data;
	}
	resetStringInfo(&v);
	appendStringInfo(&v, "%s\n%s", hex, lines);
	refs_put(key.data, key.len, &v);
	note_relations(head, lines);
	note_head_privileged(name, head, partial);
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

/* The role that made branch name, if one is recorded (never main's). */
static bool
copies_branch_creator(const char *name, Oid *role)
{
	refs_init();
	return maker_get(TAG_CREATOR, name, role);
}

static void
copies_branch_set_creator(const char *name, Oid role)
{
	refs_init();
	lock_refs();
	if (strcmp(name, "main") != 0)
		maker_put(TAG_CREATOR, name, role);
}

static void
copies_tag_set_creator(const char *name, Oid role)
{
	refs_init();
	lock_refs();
	maker_put(TAG_TAG_CREATOR, name, role);
}

static bool
copies_tag_creator(const char *name, Oid *role)
{
	refs_init();
	return maker_get(TAG_TAG_CREATOR, name, role);
}

/*
 * Branch from's record under tag of head (its hash, a newline, its lines),
 * if it is of head, branch to's as of next: NULL if not.
 */
static char *
record_of(char tag, const char *from, const ProllyHash *head)
{
	StringInfoData key;
	StringInfoData v;
	char	   *hex = prolly_hash_hex(head);
	char	   *lines = NULL;

	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, tag);
	appendStringInfoString(&key, from);
	if (refs_get(key.data, key.len, &v) &&
		strncmp(v.data, hex, strlen(hex)) == 0 && v.data[strlen(hex)] == '\n')
		lines = pstrdup(v.data + strlen(hex) + 1);
	pfree(hex);
	pfree(key.data);
	pfree(v.data);
	return lines;
}

static void
put_record(char tag, const char *to, const ProllyHash *head, const char *lines)
{
	StringInfoData key;
	StringInfoData v;
	char	   *hex = prolly_hash_hex(head);

	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, tag);
	appendStringInfoString(&key, to);
	appendStringInfo(&v, "%s\n%s", hex, lines);
	refs_put(key.data, key.len, &v);
	pfree(hex);
	pfree(key.data);
	pfree(v.data);
}

/*
 * Branch from's records of its head's relations (its tables', 'N', and
 * its other relations with privileges, 'H'), if of head, branch to's too:
 * made at that head, to's relations are from's, of the same OIDs.  So a
 * history read on to vouches for the head's tables as one on from does,
 * and their privileges carry as from's.
 */
static void
copy_head_records(const char *from, const char *to, const ProllyHash *head)
{
	const char	tags[] = {TAG_NAMES, TAG_PRIVILEGED};

	for (int i = 0; i < lengthof(tags); i++)
	{
		char	   *lines = record_of(tags[i], from, head);

		if (lines != NULL)
			put_record(tags[i], to, head, lines);
	}
}

/*
 * The relation (*relid) that branch's records of its head's relations list
 * under the qualified name name, and the commit they are of (*at).  name is
 * written as the records write it, prolly_record_relation_name()'s, and
 * compared with each line's name unescaped (relation_lines()), so a name
 * of any characters matches itself.
 */
static bool
copies_recorded_relid(const char *branch, const char *name, Oid *relid,
					  ProllyHash *at)
{
	const char	tags[] = {TAG_NAMES, TAG_PRIVILEGED};

	refs_init();
	for (int i = 0; i < lengthof(tags); i++)
	{
		StringInfoData key;
		StringInfoData v;
		char	   *nl;
		bool		found = false;

		initStringInfo(&key);
		initStringInfo(&v);
		appendStringInfoChar(&key, tags[i]);
		appendStringInfoString(&key, branch);
		if (refs_get(key.data, key.len, &v) &&
			(nl = strchr(v.data, '\n')) != NULL &&
			prolly_hash_from_hex(v.data, nl - v.data, at))
		{
			foreach_ptr(ProllyCommitRelation, r, relation_lines(nl + 1))
				if (strcmp(r->name, name) == 0)
				{
					*relid = r->relid;
					found = true;
					break;
				}
		}
		pfree(key.data);
		pfree(v.data);
		if (found)
			return true;
	}
	return false;
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
	/* (made of the head's relations, as their names say) */
	note_relations(next, lines[0] == '\n' ? lines + 1 : lines);
	{
		char	   *others = record_of(TAG_PRIVILEGED, name, head);

		if (others != NULL)
			put_record(TAG_PRIVILEGED, name, next, others);
	}
	pfree(hex);
	pfree(lines);
	pfree(key.data);
	pfree(v.data);
}

/*
 * The working set's identity anchor (the canonicalizer's identify()): what
 * tells its tables and columns by its own relations and attributes, which
 * a rename keeps, as of the branch's head (head NULL: none, a branch
 * without commits).  An anchor of another head is stale (the branch moved
 * without one made): then the working set's objects are found by name in
 * the head, which a move left it as.
 */
static void
anchor_key(StringInfo key, const char *name)
{
	appendStringInfoChar(key, TAG_IDENTITY);
	appendStringInfoString(key, name);
}

static void
note_anchor(const char *name, const ProllyHash *head, const StringInfoData *anchor)
{
	StringInfoData key;
	StringInfoData v;

	initStringInfo(&key);
	initStringInfo(&v);
	anchor_key(&key, name);
	if (head)
		appendStringInfoString(&v, prolly_hash_hex(head));
	appendStringInfoChar(&v, '\n');
	appendBinaryStringInfo(&v, anchor->data, anchor->len);
	refs_put(key.data, key.len, &v);
	pfree(key.data);
	pfree(v.data);
}

/* Branch name's anchor into *anchor, if it is of head (NULL: none). */
static bool
anchor_of(const char *name, const ProllyHash *head, StringInfo anchor)
{
	StringInfoData key;
	StringInfoData v;
	char	   *hex = head ? prolly_hash_hex(head) : "";
	bool		found;

	initStringInfo(&key);
	initStringInfo(&v);
	anchor_key(&key, name);
	found = refs_get(key.data, key.len, &v) &&
		strncmp(v.data, hex, strlen(hex)) == 0 && v.data[strlen(hex)] == '\n';
	if (found)
		appendBinaryStringInfo(anchor, v.data + strlen(hex) + 1,
							   v.len - strlen(hex) - 1);
	pfree(key.data);
	pfree(v.data);
	return found;
}

/*
 * The session branch's working identity, into *identity, and (next not
 * NULL) its next anchor: from its anchor, if it is of its head, and its
 * head's identity record (none if the branch has no commits).
 */
static void
identify_working(Oid only, StringInfo identity, StringInfo next)
{
	ProllyDatabaseComponents dc;
	char	   *name = branch_ops()->current();
	ProllyHash	head;
	bool		have_head = branch_head(name, &head);
	ProllyCommitInfo info;
	ProllyValue known = {NULL, 0};
	StringInfoData anchor;
	ProllyValue av;
	bool		anchored;

	prolly_database_components(&dc);
	if (!PROLLY_HAS(dc.canonicalizer, identify))
		return;
	if (have_head && copies_commit_get(&head, &info) && info.identity)
	{
		known.data = (const uint8 *) info.identity;
		known.len = info.identity_len;
	}
	initStringInfo(&anchor);
	anchored = anchor_of(name, have_head ? &head : NULL, &anchor);
	av.data = (const uint8 *) anchor.data;
	av.len = anchor.len;
	PROLLY_CALL(dc.canonicalizer, identify, anchored ? &av : NULL,
				known.len > 0 ? &known : NULL, only, identity, next);
	pfree(anchor.data);
}

/*
 * Anchor the session branch's working set as what identity record known
 * (NULL: every object its own name's) describes, as of its head (or head,
 * if given: the one the branch is about to have): it was just made so.
 */
static void
anchor_working(const ProllyHash *head, const ProllyValue *known)
{
	ProllyDatabaseComponents dc;
	char	   *name = branch_ops()->current();
	ProllyHash	h;
	StringInfoData identity;
	StringInfoData next;

	prolly_database_components(&dc);
	if (!PROLLY_HAS(dc.canonicalizer, identify))
		return;
	if (head == NULL && branch_head(name, &h))
		head = &h;
	initStringInfo(&identity);
	initStringInfo(&next);
	PROLLY_CALL(dc.canonicalizer, identify, NULL, known, InvalidOid,
				&identity, &next);
	note_anchor(name, head, &next);
	pfree(identity.data);
	pfree(next.data);
}

/* anchor_working() as commit's identity describes it. */
static void
anchor_at_commit(const ProllyCommitInfo *commit)
{
	ProllyValue known = {(const uint8 *) commit->identity, commit->identity_len};

	anchor_working(&commit->id, commit->identity ? &known : NULL);
}

static void
copies_working_identity(Oid only, StringInfo identity)
{
	identify_working(only, identity, NULL);
}

static void
copies_anchor_identity(const ProllyValue *identity)
{
	lock_refs();
	anchor_working(NULL, identity);
}

/*
 * Are rel's name now and its name at head the same?  at_head: the
 * relations of head_names()'s lines.
 */
static bool
named_as_at_head(List *at_head, Relation rel)
{
	char	   *name = prolly_record_relation_name(get_namespace_name(RelationGetNamespace(rel)),
												   RelationGetRelationName(rel));
	ListCell   *lc;

	foreach(lc, at_head)
	{
		ProllyCommitRelation *r = lfirst(lc);

		if (r->relid == RelationGetRelid(rel) && strcmp(r->name, name) == 0)
			return true;
	}
	return false;
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
	StringInfoData identity;
	StringInfoData anchor;
	Snapshot	snap;

	pushed = hold_for_commit(name, parents, nparents);
	/* (the fold's, which the pages kept as of the commit are asked of) */
	snap = RegisterSnapshot(fold_snapshot());
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
		UnregisterSnapshot(snap);
		return false;
	}
	initStringInfo(&identity);
	initStringInfo(&anchor);
	identify_working(InvalidOid, &identity, &anchor);
	write_commit(&root.hash, parents, nparents, prolly_commit_author(),
				 message, &identity, commit_id);
	branch_set_head(name, commit_id);
	branch_set_baseline(name, &next, &root);
	note_head_names(name, commit_id, NULL, nparents > 0 ? &parents[0] : NULL);
	note_anchor(name, commit_id, &anchor);
	keep_pages(commit_id, nparents > 0 ? &parents[0] : NULL, &root, snap);
	UnregisterSnapshot(snap);
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
find_other_case(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	OtherCase  *oc = arg;
	const char *sep = memchr(k->data, '\0', k->len);
	char	   *name;

	if (sep == NULL)
		return;
	name = pnstrdup(sep + 1, (const char *) k->data + k->len - (sep + 1));
	if (oc->found == NULL && strcmp(name, oc->name) != 0 &&
		(oc->ignore == NULL || strcmp(name, oc->ignore) != 0) &&
		pg_strcasecmp(name, oc->name) == 0)
		oc->found = name;
}

static void copies_branches(ProllyBranchVisit visit, void *arg);

/*
 * Refuse a new branch name that differs from a branch's, other than
 * ignore's (a branch being renamed), only by case, as Dolt does: on a
 * case-insensitive file system (or to a user) they're one.  Names are
 * ASCII, folded as such.  (The hidden branches of sessions checked out at
 * commits, named for them, aren't checked: no user names them.)  Only the
 * branches of the same name folded are read, from the index of names
 * folded, so a create's cost doesn't grow with the branches there are.
 */
static void
refuse_other_case(const char *name, const char *ignore)
{
	OtherCase	oc = {name, ignore, NULL};
	StringInfoData prefix;

	if (strncmp(name, PROLLY_DETACHED_PREFIX, strlen(PROLLY_DETACHED_PREFIX)) == 0)
		return;
	initStringInfo(&prefix);
	folded_key(&prefix, name);
	/* (the folded name and its NUL: every branch's of that name folded) */
	prefix.len -= strlen(name);
	visit_prefix(prefix.data, prefix.len, find_other_case, &oc);
	pfree(prefix.data);
	/* (main, which no path makes, is there from the database's start) */
	if (oc.found == NULL && strcmp(name, "main") != 0 &&
		(ignore == NULL || strcmp(ignore, "main") != 0) &&
		pg_strcasecmp(name, "main") == 0 && branch_exists("main"))
		oc.found = pstrdup("main");
	if (oc.found)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" already exists", oc.found),
				 errdetail("Branch names that differ only by case name one branch.")));
}

/* A visit of the branches a folded name's index entries name. */
typedef struct FoldedVisit
{
	ProllyBranchVisit visit;
	void	   *arg;
	bool		main;			/* main visited */
} FoldedVisit;

static void
visit_folded(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	FoldedVisit *fv = arg;
	const char *sep = memchr(k->data, '\0', k->len);
	ProllyHash	none = {0};

	if (sep != NULL)
	{
		char	   *name = pnstrdup(sep + 1, (const char *) k->data + k->len - (sep + 1));

		fv->main |= strcmp(name, "main") == 0;
		fv->visit(name, &none, fv->arg);
	}
}

/*
 * Visit the branches whose names are name but for case, by the
 * case-folded name index: the work is in those, however many branches
 * there are.  Their heads aren't read (each visited with an empty one).
 */
static void
copies_branches_folded(const char *name, ProllyBranchVisit visit, void *arg)
{
	FoldedVisit fv = {visit, arg, false};
	StringInfoData prefix;
	ProllyHash	none = {0};

	refs_init();
	initStringInfo(&prefix);
	folded_key(&prefix, name);
	prefix.len -= strlen(name);
	visit_prefix(prefix.data, prefix.len, visit_folded, &fv);
	pfree(prefix.data);
	/* (main, which no path makes, is in the index only once it has a head) */
	if (!fv.main && pg_strcasecmp(name, "main") == 0 && branch_exists("main"))
		visit("main", &none, arg);
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
	maker_put(TAG_CREATOR, name, GetUserId());

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
			folded_index_set(name, true);
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
	branch_set_head(name, &id);	/* (its name folded with its head) */
	branch_set_baseline(name, NULL, NULL);
	/* (and its start point's records of its relations, of that head) */
	if (from_exists)
		copy_head_records(from, name, &id);
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
		violations_clear(name);
		resetStringInfo(&key);
		names_key(&key, name);
		refs_put(key.data, key.len, NULL);
		resetStringInfo(&key);
		privileged_key(&key, name);
		refs_put(key.data, key.len, NULL);
		resetStringInfo(&key);
		appendStringInfoChar(&key, TAG_REBASE);
		appendStringInfoString(&key, name);
		refs_put(key.data, key.len, NULL);
		resetStringInfo(&key);
		anchor_key(&key, name);
		refs_put(key.data, key.len, NULL);
		pfree(key.data);
	}
	maker_put(TAG_CREATOR, name, InvalidOid);
	/*
	 * (and the commits its pages were kept as of, their roots let go of as
	 * the deletion commits: the page store keeps them, not the branch's
	 * storage; the storage restores replaced, kept for older snapshots,
	 * dropped, as no session is on the branch; the entry read first, for
	 * the indexes of what it holds now; and its name folded)
	 */
	if (branch_ops()->let_go)
		foreach_ptr(KeptCommit, kc, kept_get(name))
			for (int t = 0; t < kc->ntables; t++)
				for (int r = 0; r < kc->tables[t].nrels; r++)
					branch_ops()->let_go(&kc->tables[t].rels[r]);
	kept_put(name, NIL);
	folded_index_set(name, false);
	forget_read_caches();
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
	TAG_NAMES, TAG_PRIVILEGED, TAG_IDENTITY, TAG_REBASE, TAG_CREATOR,
	TAG_IDENTITY_CONFLICTS};
	ListCell   *lc;
	List	   *kept;
	ProllyNodeRef root;
	ProllyHash	head_root;
	bool		had_root;
	bool		had_head_root;

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
		/* (one with only a head: no session is on it; checked as one with copies) */
		branch_ops()->check_drop(name);
		prolly_check_branch_rename(newname);
	}
	/*
	 * (violations, kept one entry each, moved apart from the tags'; the
	 * tags' move an earlier format's value with the branch, to be refused
	 * there, not left for a branch made again of this name)
	 */
	violations_clear(newname);
	violations_move(name, newname);

	/*
	 * (the kept commits, and the indexes keyed by the branch's name, moved
	 * as their entries are: let go of under the old name, made again under
	 * the new)
	 */
	kept = kept_get(name);
	had_root = branch_baseline_root(name, &root);
	had_head_root = head_root_get(name, &head_root);
	kept_put(name, NIL);
	head_index_set(name, had_head_root ? &head_root : NULL, NULL);
	baseline_index_set(name, had_root ? &root : NULL, NULL);
	folded_index_set(name, false);
	forget_read_caches();
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
	kept_put(newname, kept);
	if (had_head_root)
		head_index_set(newname, NULL, &head_root);
	if (had_root)
		baseline_index_set(newname, NULL, &root);
	folded_index_set(newname, true);
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
 * then passing), and read every table in full.  The fold waits for no
 * relation's lock: one not free at once fails it, with
 * ERRCODE_LOCK_NOT_AVAILABLE.  The caller keeps no snapshot, active or
 * registered: the fold's is to be the oldest the transaction holds, or its
 * trim of the change logs is held back (see below).
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

	/*
	 * (the fold's snapshot the oldest the transaction keeps: one older, the
	 * caller's or the catalog snapshot the lookups above took, from while a
	 * writer the fold sees committed was running, would hold the xmin
	 * horizon below that writer, and the fold's trim of the log would keep
	 * its rows; the fold after, finding nothing new, trims nothing)
	 */
	Assert(!HaveRegisteredOrActiveSnapshot());
	InvalidateCatalogSnapshot();
	PushActiveSnapshot(GetLatestSnapshot());

	/*
	 * (never waiting for a relation's lock either: the fold keeps its locks
	 * on the change logs it consumed past its lock on their table, so a
	 * DROP of several tables, holding them and waiting for one's log, would
	 * deadlock with the fold waiting for the next table; a lock not free at
	 * once fails the fold with ERRCODE_LOCK_NOT_AVAILABLE, for the caller
	 * to roll back and fold again later)
	 */
	RelationLocksNoWait = true;
	RelationLockRefused = false;
	PG_TRY();
	{
		fold_from_baseline(root, &next, false);
	}
	PG_FINALLY();
	{
		RelationLocksNoWait = false;
	}
	PG_END_TRY();
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
 * A transaction whose snapshot can't move, coming to branch name (oid)'s
 * working set, refused if a move since its snapshot gave the branch's
 * tables the pages kept as of its head (restore_pages()): it would read
 * the head's rows, which neither its snapshot nor the branch ever had.
 * (Sessions on the branch as the move ran held it back instead.)
 */
static void
check_head_restore_seen(const char *name, Oid oid)
{
	if (IsolationUsesXactSnapshot() &&
		branch_head_restore_unseen(oid, GetTransactionSnapshot()))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access: branch \"%s\" was given its head's kept pages since this transaction's snapshot",
						name),
				 errhint("The transaction might succeed if retried.")));
}

/*
 * A read of branch name's copies (oid) without checking it out, refused
 * where a checkout would be (copies_checkout()).
 */
static void
copies_check_copies_read(const char *name, Oid oid)
{
	if (IsolationUsesXactSnapshot() && branch_made_unseen(name))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access: branch \"%s\" was first used since this transaction's snapshot",
						name),
				 errhint("The transaction might succeed if retried.")));
	check_head_restore_seen(name, oid);
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
	if (strcmp(current, name) != 0 && OidIsValid(oid))
		check_head_restore_seen(name, oid);
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
		if (ms.nschema > 0)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot commit with unresolved merge conflicts"),
					 errdetail("Table %s has a schema conflict.", ms.schema_tables[0]),
					 errhint("Abort the merge, make the definitions agree on one side, and merge again.")));
		if (ms.nidentity > 0)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot commit with unresolved merge conflicts"),
					 errdetail("Table %s has a table identity conflict.",
							   ms.identity[0].name),
					 errhint("Resolve it with dolt_conflicts_resolve('--ours') or ('--theirs'), or abort the merge.")));
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
commit_rows(const char *message, const ProllyRowChanges *changes,
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
	List	   *at_head;

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
	at_head = relation_lines(names[0] == '\n' ? names + 1 : names);
	for (int i = 0; i < nchanges; i++)
	{
		Relation	rel = try_table_open(changes[i].relid, AccessShareLock);
		bool		same = rel != NULL && named_as_at_head(at_head, rel);

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
	{
		StringInfoData identity;
		StringInfoData anchor;

		/* (each table named as at the head: its identities are the head's) */
		initStringInfo(&identity);
		if (info.identity)
			appendBinaryStringInfo(&identity, info.identity, info.identity_len);
		write_commit(&root.hash, &head, 1, prolly_commit_author(), message,
					 &identity, commit_id);
		initStringInfo(&anchor);
		if (anchor_of(name, &head, &anchor))
			note_anchor(name, commit_id, &anchor);
	}
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

	/*
	 * (the pages kept as of the commit, when a root is due: they hold the
	 * head's rows and this transaction's, if no other writer of the branch
	 * is still at work, which keep() checks by a snapshot taken now)
	 */
	if (!root_soon(name, false))
	{
		Snapshot	snap = RegisterSnapshot(GetLatestSnapshot());

		keep_pages(commit_id, &head, &root, snap);
		UnregisterSnapshot(snap);
	}
	return 1;
}

/*
 * A small commit reads the same nodes again and again: the refs' root for
 * each entry got and put, the head's commit, the root's and each table's
 * upper nodes for the seek and then the apply.  Its trees are held (the
 * branch is locked) till it ends, so it keeps the nodes it reads, and
 * reads each once.
 */
static int
copies_commit_rows(const char *message, const ProllyRowChanges *changes,
				   int nchanges, const Oid *sequences, int nsequences,
				   ProllyHash *commit_id)
{
	MemoryContext kept = NULL;
	int			r;

	refs_init();
	if (PROLLY_HAS(tree, keep_nodes))
	{
		kept = AllocSetContextCreate(CurrentMemoryContext, "commit's kept nodes",
									 ALLOCSET_DEFAULT_SIZES);
		PROLLY_CALL(tree, keep_nodes, kept);
	}
	PG_TRY();
	{
		r = commit_rows(message, changes, nchanges, sequences, nsequences,
						commit_id);
	}
	PG_FINALLY();
	{
		if (kept)
			MemoryContextDelete(kept);
	}
	PG_END_TRY();
	return r;
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
	StringInfoData identity;
	StringInfoData anchor;
	Snapshot	snap;

	if (working_set == NULL)
	{
		(void) commit_branch(message, parents, nparents, true, commit_id);
		return;
	}
	name = branch_ops()->current();
	pushed = hold_for_commit(name, parents, nparents);
	snap = RegisterSnapshot(fold_snapshot());
	fold_from_baseline(&root, &next, true);
	if (pushed)
		PopActiveSnapshot();
	initStringInfo(&identity);
	initStringInfo(&anchor);
	identify_working(InvalidOid, &identity, &anchor);
	write_commit(&working_set->hash, parents, nparents, prolly_commit_author(),
				 message, &identity, commit_id);
	branch_set_head(name, commit_id);
	branch_set_baseline(name, &next, &root);
	/* (the working set committed, or a move's result over changes of its own) */
	note_head_names(name, commit_id,
					same_hash(&root.hash, &working_set->hash) ? NULL :
					nparents > 0 ? &parents[0] : &(ProllyHash) {0},
					nparents > 0 ? &parents[0] : NULL);
	note_anchor(name, commit_id, &anchor);
	/* (kept only if the working set is what was committed) */
	if (same_hash(&root.hash, &working_set->hash))
		keep_pages(commit_id, nparents > 0 ? &parents[0] : NULL, &root, snap);
	UnregisterSnapshot(snap);
}

static void
copies_set_head(const ProllyHash *commit_id)
{
	char	   *name = branch_ops()->current();
	ProllyHash	head;
	bool		have_head;
	StringInfoData anchor;
	bool		anchored;

	lock_refs();
	have_head = branch_head(name, &head);
	initStringInfo(&anchor);
	anchored = anchor_of(name, have_head ? &head : NULL, &anchor);
	branch_set_head(name, commit_id);

	/*
	 * (the working set's relations, and what they are, are as they were:
	 * its anchor is still theirs, now as of the head moved to)
	 */
	if (anchored)
		note_anchor(name, commit_id, &anchor);
	pfree(anchor.data);
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
	visit_head_only(NULL, visit_head_only_branch, &vb);
}

/* The same, of the branches named with prefix alone. */
static void
copies_branches_named(const char *prefix, ProllyBranchVisit visit, void *arg)
{
	VisitBranches vb = {visit, arg};

	refs_init();
	branch_ops()->branches_named(prefix, visit_branch, &vb);
	visit_head_only(prefix, visit_head_only_branch, &vb);
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

/* The canonicalizer's pending_full() of the session branch, from its baseline. */
static void
copies_pending_full_folds(ProllyPendingFullVisit visit, void *arg)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef baseline;
	bool		have;

	refs_init();
	prolly_database_components(&dc);
	have = branch_baseline(branch_ops()->current(), &baseline);
	PROLLY_CALL(dc.canonicalizer, pending_full, have ? &baseline : NULL,
				visit, arg);
}

/*
 * The last fold of the session branch known (branch_fold_last()), from
 * its baseline now, and when it was known; never one only this backend's
 * chunks hold, nor one a fold forced to read would skip.
 */
static bool
copies_last_fold(Snapshot snap, ProllyNodeRef *root, ProllyFoldMark *mark)
{
	ProllyNodeRef baseline;
	ProllyHash	h;
	bool		have;

	if (!fold_caches_usable() || prolly_local_chunks_active() ||
		!OidIsValid(MyBranch))
		return false;
	have = branch_baseline(branch_ops()->current(), &baseline);
	if (!branch_fold_last(MyBranch, snap, have ? &baseline.hash : NULL, &h,
						  mark))
		return false;
	ref_of(&h, root);
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
	check_head_restore_seen(branch, branch_oid(branch));
	branch_ops()->run_on(branch, fold_into, root);
	return true;
}

/*
 * Make the commit ref names the session branch's head, and move the branch
 * there: its rows written as the commit has them.
 */
/* The tables of kept commit kc a restore would give its pages, planned. */
static void
plan_restore(const ProllyDatabaseComponents *dc, const KeptCommit *kc,
			 const ProllyNodeRef *root, const ProllyNodeRef *moving_to,
			 const ProllyNodeRef *baseline, List **planned, List **relids)
{
	List	   *tables = NIL;
	List	   *readable = NIL;
	List	   *targets = NIL;

	PROLLY_CALL(dc->canonicalizer, tables, root, NULL, note_table, &tables);
	foreach_ptr(NamedTree, t, tables)
	{
		if (t->readable)
			readable = lappend(readable, t);
	}
	if (moving_to != NULL)
		PROLLY_CALL(dc->canonicalizer, tables, moving_to, NULL, note_table,
					&targets);
	for (int t = 0; t < kc->ntables; t++)
	{
		const KeptTable *kt = &kc->tables[t];
		Oid			relid;
		bool		skipped = false;

		foreach_ptr(char, n, *planned)
		{
			if (strcmp(n, kt->name) == 0)
				skipped = true;
		}
		if (skipped ||
			!restore_planned(dc, kt, readable, targets, moving_to != NULL,
							 baseline, NULL, &relid) ||
			!branch_ops()->changed_since_kept(kt->rels, kt->nrels))
			continue;
		*planned = lappend(*planned, kt->name);

		/*
		 * (a partition's ancestors first, top down, as a query on them and
		 * the move lock them before their partitions)
		 */
		if (get_rel_relispartition(relid))
		{
			List	   *ancestors = get_partition_ancestors(relid);

			for (int a = list_length(ancestors) - 1; a >= 0; a--)
				*relids = list_append_unique_oid(*relids,
												 list_nth_oid(ancestors, a));
		}
		for (int r = 0; r < kt->nrels; r++)
			*relids = list_append_unique_oid(*relids, kt->rels[r].relid);
	}
}

/*
 * Before a hard reset holds the refs (which every other branch's
 * operation waits for): the tables a restore will give the pages kept as
 * of the commit ref names, or of the head, locked as TRUNCATE locks them
 * (planned as restore_pages() plans: only those it gives pages, not those
 * whose rows it writes, which take a writer's locks).  So a transaction
 * that read one and is still open (on main, which sessions don't lock,
 * any) holds up the reset alone, and the restore finds the locks held.
 *
 * The lock order, against every other operation's (the branch's refs,
 * then the root, then the relations it reads or writes):
 * - the tables' locks all or none: those free taken, and the first one
 *   that isn't queued for with nothing else held, then all tried again
 *   (as lmgr.c's callers of ConditionalLockRelationOid() take sets), so
 *   the reset never holds one table while it waits for another;
 * - then the branch's refs and the root, only tried: if either is held,
 *   every table's lock is given back, and the reset waits for each in its
 *   queue, holding nothing (taking it and letting go of it at once), and
 *   begins again.
 * The planning itself takes no lock it keeps (may_restore() and
 * kept_relations() let go of what they take to read a relation), so two
 * resets of one branch at once don't each hold a table the other waits
 * to lock.
 * So it never holds a lock another operation waits for while it waits
 * for one that operation holds.  Read as the refs are now: whatever
 * changes before they are held the restore tries for, as without this.
 * Not once the refs are held already (a transaction holding them waits
 * for no relation lock here: its restores try theirs).
 */
static void
lock_for_restore(const char *ref)
{
	ProllyDatabaseComponents dc;
	char	   *name;
	List	   *kept;
	List	   *planned = NIL;
	List	   *relids = NIL;
	ProllyHash	head;
	ProllyHash	target;
	ProllyCommitInfo info;
	ProllyCommitInfo at_head;
	ProllyNodeRef baseline;
	KeptCommit *was;
	Oid			waited = InvalidOid;

	if (whole || prolly_kept_commits <= 0 ||
		branch_ops()->changed_since_kept == NULL || RecoveryInProgress())
		return;
	prolly_database_components(&dc);
	name = branch_ops()->current();
	kept = kept_get(name);
	if (kept == NIL || !branch_baseline(name, &baseline) ||
		!copies_resolve(ref, &target) || !copies_commit_get(&target, &info))
		return;
	was = kept_find(kept, &target);
	if (was != NULL && was->baseline.len > 0)
		plan_restore(&dc, was, &info.root, NULL, &baseline, &planned, &relids);
	if (branch_head(name, &head) && !same_hash(&head, &target) &&
		(was = kept_find(kept, &head)) != NULL && was->baseline.len > 0 &&
		copies_commit_get(&head, &at_head))
		plan_restore(&dc, was, &at_head.root, &info.root, &baseline,
					 &planned, &relids);
	/* (read again once the refs are held) */
	kept_read_name = NULL;
	if (relids == NIL)
		return;

	for (;;)
	{
		bool		all = true;
		ListCell   *lc;
		int			ntaken = 0;

		CHECK_FOR_INTERRUPTS();
		foreach(lc, relids)
		{
			if (!ConditionalLockRelationOid(lfirst_oid(lc), AccessExclusiveLock))
			{
				all = false;
				break;
			}
			ntaken++;
		}
		/* (the lock waited for, held once more than the set needs) */
		if (OidIsValid(waited))
			UnlockRelationOid(waited, AccessExclusiveLock);
		waited = InvalidOid;
		if (all)
		{
			if (try_lock_branch(name, true))
			{
				if (!PROLLY_HAS(env.chunk_store, try_hold_root) ||
					env.chunk_store->try_hold_root(env.chunks))
					break;
				release_branch(name);
			}
			/*
			 * (the refs held: everything given back, and each waited for
			 * in its queue, holding nothing, as the deadlock detector and
			 * lock_timeout see, then let go of, to try again)
			 */
			foreach(lc, relids)
				UnlockRelationOid(lfirst_oid(lc), AccessExclusiveLock);
			lock_branch(name);
			release_branch(name);
			if (PROLLY_HAS(env.chunk_store, wait_root_free))
				env.chunk_store->wait_root_free(env.chunks);
			continue;
		}
		/* (those taken given back, and the busy one queued for alone) */
		for (int i = 0; i < ntaken; i++)
			UnlockRelationOid(list_nth_oid(relids, i), AccessExclusiveLock);
		waited = list_nth_oid(relids, ntaken);
		LockRelationOid(waited, AccessExclusiveLock);
	}
	AcceptInvalidationMessages();
}

static void
copies_reset(const char *ref)
{
	ProllyHash	target;
	ProllyCommitInfo info;
	bool		pushed;

	lock_for_restore(ref);
	lock_refs();
	if (!copies_resolve(ref, &target) || !copies_commit_get(&target, &info))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	/* (the rows moved as a snapshot that sees the head has them) */
	pushed = see_head(branch_ops()->current());
	move_settled(&info);
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
 * each table's OID, then its name's length (a varint) and bytes.  Then the
 * tables in schema conflicts, by name alone (OID 0: one the branch dropped
 * has none).
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
	for (int i = 0; state && i < state->nschema; i++)
	{
		const char *base = state->schema_base_tables ?
			state->schema_base_tables[i] : state->schema_tables[i];
		const char *theirs = state->schema_their_tables ?
			state->schema_their_tables[i] : state->schema_tables[i];

		/* (ours' name, then base's and theirs', a NUL before each) */
		put_be32(&v, InvalidOid);
		prolly_put_varint(&v, strlen(state->schema_tables[i]) + 1 +
						  strlen(base) + 1 + strlen(theirs));
		appendStringInfoString(&v, state->schema_tables[i]);
		appendStringInfoChar(&v, '\0');
		appendStringInfoString(&v, base);
		appendStringInfoChar(&v, '\0');
		appendStringInfoString(&v, theirs);
		any = true;
	}
	{
		StringInfoData had;

		initStringInfo(&had);
		if (any || refs_get(key.data, key.len, &had))
			refs_put(key.data, key.len, any ? &v : NULL);
		pfree(had.data);
	}
	identity_conflicts_put(branch, state);
	pfree(key.data);
	pfree(v.data);
}

/*
 * A branch's merge's table identity conflicts (TAG_IDENTITY_CONFLICTS):
 * the local branch theirs was, then each table's relations, tree and
 * names.  None: no entry.
 */
static void
identity_conflicts_put(const char *branch, const ProllyMergeState *state)
{
	StringInfoData key;
	StringInfoData v;
	StringInfoData had;
	bool		any = state != NULL &&
		(state->nidentity > 0 ||
		 (state->identity_saved != NULL && state->identity_saved[0] != '\0'));

	initStringInfo(&key);
	initStringInfo(&v);
	initStringInfo(&had);
	appendStringInfoChar(&key, TAG_IDENTITY_CONFLICTS);
	appendStringInfoString(&key, branch);
	if (any)
	{
		const char *source = state->identity_source ? state->identity_source : "";

		const char *saved = state->identity_saved ? state->identity_saved : "";

		prolly_put_varint(&v, strlen(source));
		appendStringInfoString(&v, source);
		prolly_put_varint(&v, strlen(saved));
		appendStringInfoString(&v, saved);
		for (int i = 0; i < state->nidentity; i++)
		{
			const ProllyIdentityConflict *c = &state->identity[i];

			put_be32(&v, c->relid);
			put_be32(&v, c->their_relid);
			prolly_put_varint(&v, c->tree);
			prolly_put_varint(&v, strlen(c->name));
			appendStringInfoString(&v, c->name);
			prolly_put_varint(&v, strlen(c->their_name));
			appendStringInfoString(&v, c->their_name);
		}
	}
	if (any || refs_get(key.data, key.len, &had))
		refs_put(key.data, key.len, any ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
	pfree(had.data);
}

/* A varint length, then that many bytes, from *p (end: the value's end). */
static char *
identity_conflicts_string(const char **p, const char *end)
{
	uint64		len = prolly_get_varint(p, end);
	char	   *out;

	if (len > (uint64) (end - *p))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly refs hold an invalid merge state")));
	out = pnstrdup(*p, len);
	*p += len;
	return out;
}

/* state's identity conflicts, from identity_conflicts_put()'s entry */
static void
identity_conflicts_get(const char *branch, ProllyMergeState *state)
{
	StringInfoData key;
	StringInfoData v;

	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_IDENTITY_CONFLICTS);
	appendStringInfoString(&key, branch);
	state->nidentity = 0;
	state->identity = NULL;
	state->identity_source = NULL;
	state->identity_saved = NULL;
	if (refs_get(key.data, key.len, &v))
	{
		const char *p = v.data;
		const char *end = v.data + v.len;
		int			cap = 4;

		state->identity_source = identity_conflicts_string(&p, end);
		if (state->identity_source[0] == '\0')
			state->identity_source = NULL;
		state->identity_saved = identity_conflicts_string(&p, end);
		state->identity = palloc(sizeof(ProllyIdentityConflict) * cap);
		while (p < end)
		{
			ProllyIdentityConflict *c;

			if (end - p < 8)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("prolly refs hold an invalid merge state")));
			if (state->nidentity == cap)
			{
				cap *= 2;
				state->identity = repalloc(state->identity,
										   sizeof(ProllyIdentityConflict) * cap);
			}
			c = &state->identity[state->nidentity++];
			c->relid = get_be32(p);
			c->their_relid = get_be32(p + 4);
			p += 8;
			c->tree = (int) prolly_get_varint(&p, end);
			c->name = identity_conflicts_string(&p, end);
			c->their_name = identity_conflicts_string(&p, end);
		}
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
	state->nschema = 0;
	state->schema_tables = NULL;
	state->schema_base_tables = NULL;
	state->schema_their_tables = NULL;
	if (refs_get(key.data, key.len, &v))
	{
		const char *p = v.data;
		const char *end = v.data + v.len;
		int			cap = 4;

		state->table_names = palloc0(sizeof(char *) * Max(state->ntables, 1));
		state->schema_tables = palloc(sizeof(char *) * cap);
		state->schema_base_tables = palloc(sizeof(char *) * cap);
		state->schema_their_tables = palloc(sizeof(char *) * cap);
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
			if (oid == InvalidOid)
			{
				char	   *names = palloc(len + 1);
				char	   *base;
				char	   *theirs;

				memcpy(names, p, len);
				names[len] = '\0';
				base = names + strlen(names) + 1;

				if (base > names + len)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("prolly refs hold an invalid merge state")));
				theirs = base + strlen(base) + 1;
				if (theirs > names + len)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("prolly refs hold an invalid merge state")));
				if (state->nschema == cap)
				{
					cap *= 2;
					state->schema_tables = repalloc(state->schema_tables,
													sizeof(char *) * cap);
					state->schema_base_tables = repalloc(state->schema_base_tables,
														 sizeof(char *) * cap);
					state->schema_their_tables = repalloc(state->schema_their_tables,
														  sizeof(char *) * cap);
				}
				state->schema_base_tables[state->nschema] = base;
				state->schema_their_tables[state->nschema] = theirs;
				state->schema_tables[state->nschema++] = names;
			}
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
	{
		merge_names_get(branch, state);
		identity_conflicts_get(branch, state);
	}
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

/*
 * A branch's constraint violations.  They are kept one entry each, keyed
 * by the tag, the branch's name, a zero byte and the entry's place (eight
 * bytes, big-endian), after an entry of that prefix alone, which holds the
 * next place: so violations are added at the end without the others being
 * read or written again, and read back in the order they were added.
 * (A branch's name holds no zero byte, so no key is another branch's.)
 * Format version 1 kept them all in one value, keyed by the tag and the
 * branch's name alone; such a value isn't read, but refused, and is
 * removed when the branch's violations are cleared (a reset).
 */

#define VIOLATIONS_BATCH		1000

static void
violations_key(StringInfo key, const char *branch)
{
	appendStringInfoChar(key, TAG_VIOLATIONS);
	appendStringInfoString(key, branch);
}

static void
violations_prefix(StringInfo key, const char *branch)
{
	violations_key(key, branch);
	appendStringInfoChar(key, '\0');
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

static void
put_violation(StringInfo out, const ProllyViolation *pv)
{
	put_be32(out, pv->relid);
	put_text(out, pv->type);
	put_text(out, pv->row);
	put_text(out, pv->info);
}

static void
get_violation(const char **p, const char *end, ProllyViolation *pv)
{
	if (end - *p < 4)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a branch's constraint violations are corrupt")));
	pv->relid = get_be32(*p);
	*p += 4;
	pv->type = get_text(p, end);
	pv->row = get_text(p, end);
	pv->info = get_text(p, end);
}

static void
put_be64(StringInfo out, uint64 v)
{
	put_be32(out, (uint32) (v >> 32));
	put_be32(out, (uint32) v);
}

static uint64
get_be64(const char *p)
{
	return ((uint64) get_be32(p) << 32) | get_be32(p + 4);
}

/*
 * Visit each constraint violation a merge left on branch, in the order
 * they were added, those of tables since dropped too.  Each is good only
 * during its visit.
 */
static void
violations_visit(const char *branch, ProllyViolationVisit visit, void *arg)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "branch violations",
											  ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	StringInfoData key;
	ProllyNodeRef root;

	refs_init();
	initStringInfo(&key);
	violations_key(&key, branch);
	if (refs_root(&root))
	{
		ProllyTreeCursor *c = PROLLY_CALL(tree, cursor_open, &env, &root, cxt);
		ProllyKey	from = {(const uint8 *) key.data, key.len};
		ProllyKey	k;
		ProllyValue val;

		/*
		 * (the seek starts at the earlier format's key, which comes right
		 * before the entries', so one written so is seen, and refused)
		 */
		PROLLY_CALL(tree, cursor_seek, c, &from, ForwardScanDirection);
		while (PROLLY_CALL(tree, cursor_next, c, &k, &val) &&
			   k.len >= key.len && memcmp(k.data, key.data, key.len) == 0 &&
			   (k.len == key.len || k.data[key.len] == '\0'))
		{
			const char *p = (const char *) val.data;
			ProllyViolation pv;

			if (k.len == key.len)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("the constraint violations of branch \"%s\" are kept in an earlier format",
								branch),
						 errdetail("This build reads only format version 2 of a branch's violations."),
						 errhint("Reset the branch with dolt_reset('--hard') to drop them.")));
			/* (the prefix alone holds the next place) */
			if (k.len == key.len + 1)
				continue;
			get_violation(&p, p + val.len, &pv);
			MemoryContextSwitchTo(old);
			visit(&pv, arg);
			MemoryContextSwitchTo(cxt);
			pfree(pv.type);
			pfree(pv.row);
			pfree(pv.info);
			CHECK_FOR_INTERRUPTS();
		}
		PROLLY_CALL(tree, cursor_close, c);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

static void
collect_violation(const ProllyViolation *pv, void *arg)
{
	List	  **list = arg;
	ProllyViolation *copy = palloc(sizeof(ProllyViolation));

	copy->relid = pv->relid;
	copy->type = pstrdup(pv->type);
	copy->row = pstrdup(pv->row);
	copy->info = pstrdup(pv->info);
	*list = lappend(*list, copy);
}

/*
 * The constraint violations a merge left on branch, as the refs hold them:
 * violations_left() leaves out those of tables since dropped.
 */
static List *
violations_of(const char *branch)
{
	List	   *list = NIL;

	violations_visit(branch, collect_violation, &list);
	return list;
}

/* Apply edits (n of them, in key order) to the refs, with the transaction. */
static void
refs_apply(ProllyEdit *edits, int n)
{
	ProllyNodeRef root;
	ProllyNodeRef new_root;

	if (n == 0)
		return;
	lock_refs();
	if (!refs_root(&root))
		elog(ERROR, "prolly refs have no root");
	PROLLY_CALL(tree, apply, &env, &root, edits, n, &new_root);
	if (!PROLLY_CALL(env.chunk_store, move_root, env.chunks, &root.hash,
					 &new_root.hash))
		elog(ERROR, "prolly refs moved while held");
}

/*
 * The entries under branch's prefix (their keys copied, at most max of
 * them, from the first), into *keys; returns how many.
 */
static int
violation_entries(const char *branch, StringInfoData *keys, int max)
{
	StringInfoData prefix;
	ProllyNodeRef root;
	int			n = 0;

	if (!refs_root(&root))
		return 0;
	initStringInfo(&prefix);
	violations_prefix(&prefix, branch);
	{
		ProllyTreeCursor *c = PROLLY_CALL(tree, cursor_open, &env, &root,
										  CurrentMemoryContext);
		ProllyKey	from = {(const uint8 *) prefix.data, prefix.len};
		ProllyKey	k;
		ProllyValue val;

		PROLLY_CALL(tree, cursor_seek, c, &from, ForwardScanDirection);
		while (n < max && PROLLY_CALL(tree, cursor_next, c, &k, &val) &&
			   k.len >= prefix.len &&
			   memcmp(k.data, prefix.data, prefix.len) == 0)
		{
			initStringInfo(&keys[n]);
			appendBinaryStringInfo(&keys[n], (const char *) k.data, k.len);
			n++;
		}
		PROLLY_CALL(tree, cursor_close, c);
	}
	pfree(prefix.data);
	return n;
}

/* Remove branch's constraint violations (an earlier format's value too). */
static void
violations_clear(const char *branch)
{
	StringInfoData key;
	StringInfoData v;
	StringInfoData *keys = palloc_array(StringInfoData, VIOLATIONS_BATCH);
	ProllyEdit *edits = palloc0_array(ProllyEdit, VIOLATIONS_BATCH);
	int			n;

	refs_init();
	initStringInfo(&key);
	initStringInfo(&v);
	violations_key(&key, branch);
	if (refs_get(key.data, key.len, &v))
		refs_put(key.data, key.len, NULL);
	while ((n = violation_entries(branch, keys, VIOLATIONS_BATCH)) > 0)
	{
		for (int i = 0; i < n; i++)
		{
			edits[i].key.data = (const uint8 *) keys[i].data;
			edits[i].key.len = keys[i].len;
			edits[i].is_delete = true;
		}
		refs_apply(edits, n);
		for (int i = 0; i < n; i++)
			pfree(keys[i].data);
		CHECK_FOR_INTERRUPTS();
	}
	pfree(keys);
	pfree(edits);
	pfree(key.data);
	pfree(v.data);
}

/* Add violations (n of them) to branch's, after those it has. */
static void
violations_append(const char *branch, ProllyViolation *const *violations,
				  int n)
{
	StringInfoData prefix;
	StringInfoData next;
	StringInfoData *keys;
	StringInfoData *values;
	ProllyEdit *edits;
	uint64		place = 0;

	if (n == 0)
		return;
	refs_init();
	lock_refs();
	initStringInfo(&prefix);
	initStringInfo(&next);
	violations_prefix(&prefix, branch);
	if (refs_get(prefix.data, prefix.len, &next))
	{
		if (next.len != 8)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a branch's constraint violations are corrupt")));
		place = get_be64(next.data);
	}
	keys = palloc_array(StringInfoData, n);
	values = palloc_array(StringInfoData, n);
	edits = palloc0_array(ProllyEdit, n + 1);
	resetStringInfo(&next);
	put_be64(&next, place + n);
	edits[0].key.data = (const uint8 *) prefix.data;
	edits[0].key.len = prefix.len;
	edits[0].value.data = (const uint8 *) next.data;
	edits[0].value.len = next.len;
	for (int i = 0; i < n; i++)
	{
		/* (sized to them: a batch holds as many as its rows' bytes allow) */
		initStringInfoExt(&keys[i], prefix.len + 8 + 1);
		appendBinaryStringInfo(&keys[i], prefix.data, prefix.len);
		put_be64(&keys[i], place + i);
		initStringInfoExt(&values[i], 4 + 3 * 4 + strlen(violations[i]->type) +
						  strlen(violations[i]->row) +
						  strlen(violations[i]->info) + 1);
		put_violation(&values[i], violations[i]);
		edits[i + 1].key.data = (const uint8 *) keys[i].data;
		edits[i + 1].key.len = keys[i].len;
		edits[i + 1].value.data = (const uint8 *) values[i].data;
		edits[i + 1].value.len = values[i].len;
	}
	refs_apply(edits, n + 1);
	for (int i = 0; i < n; i++)
	{
		pfree(keys[i].data);
		pfree(values[i].data);
	}
	pfree(keys);
	pfree(values);
	pfree(edits);
	pfree(prefix.data);
	pfree(next.data);
	note_merge_changed(branch);
}

/* Move branch from's constraint violations to branch to (none there). */
static void
violations_move(const char *from, const char *to)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "violations moved",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	StringInfoData *keys = palloc_array(StringInfoData, VIOLATIONS_BATCH);
	StringInfoData *values = palloc_array(StringInfoData, VIOLATIONS_BATCH);
	ProllyEdit *edits = palloc0_array(ProllyEdit, VIOLATIONS_BATCH);
	StringInfoData from_prefix;
	StringInfoData to_prefix;
	int			n;

	initStringInfo(&from_prefix);
	initStringInfo(&to_prefix);
	violations_prefix(&from_prefix, from);
	violations_prefix(&to_prefix, to);
	while ((n = violation_entries(from, keys, VIOLATIONS_BATCH)) > 0)
	{
		for (int i = 0; i < n; i++)
		{
			initStringInfo(&values[i]);
			if (!refs_get(keys[i].data, keys[i].len, &values[i]))
				elog(ERROR, "a branch's constraint violation went missing");
			edits[i].key.data = (const uint8 *) keys[i].data;
			edits[i].key.len = keys[i].len;
			edits[i].is_delete = true;
			edits[i].value.data = NULL;
			edits[i].value.len = 0;
		}
		refs_apply(edits, n);
		for (int i = 0; i < n; i++)
		{
			StringInfoData k;

			/* (the same places, so the same order, under the new name) */
			initStringInfo(&k);
			appendBinaryStringInfo(&k, to_prefix.data, to_prefix.len);
			appendBinaryStringInfo(&k, keys[i].data + from_prefix.len,
								   keys[i].len - from_prefix.len);
			pfree(keys[i].data);
			keys[i] = k;
			edits[i].key.data = (const uint8 *) keys[i].data;
			edits[i].key.len = keys[i].len;
			edits[i].is_delete = false;
			edits[i].value.data = (const uint8 *) values[i].data;
			edits[i].value.len = values[i].len;
		}
		refs_apply(edits, n);
		MemoryContextSwitchTo(old);
		MemoryContextReset(cxt);
		MemoryContextSwitchTo(cxt);
		keys = palloc_array(StringInfoData, VIOLATIONS_BATCH);
		values = palloc_array(StringInfoData, VIOLATIONS_BATCH);
		edits = palloc0_array(ProllyEdit, VIOLATIONS_BATCH);
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
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
copies_visit_violations(ProllyViolationVisit visit, void *arg)
{
	violations_visit(branch_ops()->current(), visit, arg);
}

static void
copies_add_violations(ProllyViolation *const *violations, int n)
{
	violations_append(branch_ops()->current(), violations, n);
}

/*
 * Set the session branch's violations: those it had removed, these added,
 * a batch at a time.
 */
static void
copies_set_violations(List *violations)
{
	const char *branch = branch_ops()->current();
	ProllyViolation **batch = palloc_array(ProllyViolation *, VIOLATIONS_BATCH);
	int			n = 0;

	refs_init();
	violations_clear(branch);
	foreach_ptr(ProllyViolation, pv, violations)
	{
		batch[n++] = pv;
		if (n == VIOLATIONS_BATCH)
		{
			violations_append(branch, batch, n);
			n = 0;
		}
	}
	violations_append(branch, batch, n);
	pfree(batch);
	note_merge_changed(branch);
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
 * branches' heads, remote-tracking branches' commits and tags' commits, in
 * the order of their full names (as Dolt's: a tracking branch's key, remote
 * then branch, sorts otherwise where one remote's name begins another's).
 * A tag's commit is the one its entry keeps.
 */
static bool
copies_root_refs(const ProllyHash *root, ProllyRootRefVisit visit, void *arg)
{
	static const char tags[] = {TAG_BRANCH, TAG_TRACKING, TAG_TAG};
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
			if (tags[t] == TAG_BRANCH || tags[t] == TAG_TAG)
			{
				appendStringInfoString(&ref, tags[t] == TAG_BRANCH ?
									   "refs/heads/" : "refs/tags/");
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
			{
				const char *p = (const char *) v.data;
				const char *end = p + v.len;

				p = get_hash(p, end, &refs[nrefs].commit);
				/* (a tag's object, then its commit) */
				if (tags[t] == TAG_TAG)
					(void) get_hash(p, end, &refs[nrefs].commit);
			}
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
	tracking_log_note(remote, branch, commit_id);
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

		/*
		 * its remote-tracking branches, their head logs, and the upstreams
		 * naming it go too
		 */
		initStringInfo(&prefix);
		tracking_key(&prefix, name, NULL);
		visit_prefix(prefix.data, prefix.len, collect_key, &keys);
		prefix.data[0] = TAG_TRACKING_LOG;
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
 * commits seen.  A commit missing from the store is an error; a tag object
 * is walked, then its commit; another chunk that isn't a commit is passed
 * over, nothing of it visited.
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
		{
			/*
			 * (one past the history horizon: whole where the visitor
			 * already has it, as a remote it is sent to may; else the
			 * walk can't go on)
			 */
			if (prolly_window_archived(next))
			{
				ref_of(next, &ref);
				if (!ws->visit(&ref, 0, ws->arg))
					continue;
				prolly_window_check_missing(next);
			}
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("commit %s is missing from the chunk store",
							prolly_hash_hex(next))));
		}
		if (!copies_commit_get(next, &info))
		{
			ProllyTagInfo tag;

			/* (a tag the visitor declines: it has seen its commit) */
			ref_of(next, &ref);
			if (copies_tag_get(next, &tag) &&
				PROLLY_CALL(tree, walk, &env, &ref, ws->visit, ws->arg))
			{
				ProllyHash *c = palloc(sizeof(ProllyHash));

				*c = tag.commit;
				todo = lappend(todo, c);
			}
			continue;
		}

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
 * branch, a merge in progress, a stash or a tag (with the tag's object),
 * with its root, and every baseline.
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

		/* (a rebase: the head it began at, its onto, its steps' commits) */
		if (k.len >= 1 && k.data[0] == TAG_REBASE)
		{
			ProllyRebaseState rs;

			if (rebase_decode(p, end, &rs))
			{
				heads = lappend(heads, copy_hash_of(&rs.head));
				heads = lappend(heads, copy_hash_of(&rs.onto));
				for (int i = 0; i < rs.nsteps; i++)
					heads = lappend(heads, copy_hash_of(&rs.steps[i].commit));
			}
			continue;
		}
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
				if (blen + MAKER_RECORD_LEN > (uint64) (end - p))
					break;
				p += blen + MAKER_RECORD_LEN;
			}
			continue;
		}
		if (k.len < 1 || (k.data[0] != TAG_BRANCH && k.data[0] != TAG_MERGE &&
						  k.data[0] != TAG_BASELINE && k.data[0] != TAG_TRACKING &&
						  k.data[0] != TAG_TAG))
			continue;
		/* (a tag: its object, walked as a head is, then its commit) */
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
	violations_clear(name);
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
			else
			{
				MoveSettled ms = {&info, *head};

				if (strcmp(name, branch_ops()->current()) == 0)
					move_settled_checked(&ms);
				else
					branch_ops()->run_on(name, move_settled_checked, &ms);
			}
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
 * Tags: TAG_TAG and a tag's name -> its tag object, then its commit.
 * ------------------------------------------------------------------------
 */

static void
tag_key(const char *name, StringInfo key)
{
	appendStringInfoChar(key, TAG_TAG);
	appendStringInfoString(key, name);
}

/*
 * The tag called name: its object and its commit (each if not NULL); false
 * if there is none.
 */
static bool
tag_entry(const char *name, ProllyHash *tag, ProllyHash *commit)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;

	initStringInfo(&key);
	initStringInfo(&v);
	tag_key(name, &key);
	found = refs_get(key.data, key.len, &v);
	if (found)
	{
		const char *p = v.data;
		const char *end = v.data + v.len;
		ProllyHash	t;
		ProllyHash	c;

		p = get_hash(p, end, &t);
		(void) get_hash(p, end, &c);
		if (tag)
			*tag = t;
		if (commit)
			*commit = c;
	}
	pfree(key.data);
	pfree(v.data);
	return found;
}

/*
 * Write a tag object of commit; its id is its tree's root hash.  It is
 * dated now, as a commit is.
 */
static void
copies_tag_write(const ProllyHash *commit, const char *tagger,
				 const char *message, ProllyHash *tag)
{
	ProllyCommitInfo info;
	ProllyTreeBuilder *b;
	StringInfoData v;
	ProllyNodeRef ref;

	refs_init();
	if (!copies_commit_get(commit, &info))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("commit %s does not exist", prolly_hash_hex(commit))));
	b = PROLLY_CALL(tree, build_begin, &commit_env, CurrentMemoryContext);

	/* Keys in order: commit, message, tagger, time. */
	initStringInfo(&v);
	put_hash(&v, commit);
	add_item(b, "commit", &v);
	resetStringInfo(&v);
	appendStringInfoString(&v, message);
	add_item(b, "message", &v);
	resetStringInfo(&v);
	appendStringInfoString(&v, tagger);
	add_item(b, "tagger", &v);
	resetStringInfo(&v);
	put_uint64(&v, (uint64) GetCurrentTimestamp());
	add_item(b, "time", &v);
	PROLLY_CALL(tree, build_finish, b, &ref);
	*tag = ref.hash;
	pfree(v.data);
}

/* A tag object, read; false if tag isn't one (a commit, say). */
static bool
copies_tag_get(const ProllyHash *tag, ProllyTagInfo *info)
{
	ProllyNodeRef ref;
	StringInfoData v;

	refs_init();
	if (tag->len == 0 || !PROLLY_CALL(env.chunk_store, has, env.chunks, tag))
		return false;
	ref_of(tag, &ref);
	initStringInfo(&v);
	if (!read_item(&ref, "commit", &v))
	{
		pfree(v.data);
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = *tag;
	(void) get_hash(v.data, v.data + v.len, &info->commit);
	resetStringInfo(&v);
	(void) read_item(&ref, "tagger", &v);
	info->tagger = pstrdup(v.data);
	resetStringInfo(&v);
	(void) read_item(&ref, "message", &v);
	info->message = pstrdup(v.data);
	resetStringInfo(&v);
	if (read_item(&ref, "time", &v))
		(void) get_time(&v, &info->tagged_at);
	pfree(v.data);
	return true;
}

static bool
copies_tag_lookup(const char *name, ProllyHash *tag)
{
	refs_init();
	return tag_entry(name, tag, NULL);
}

/*
 * Call name tag (NULL: delete it), if the tag called name is expected's
 * (NULL: whatever it is; an empty hash: there is none).  The refs are held
 * first, so no other session's change comes between the check and the
 * change.
 */
static bool
copies_tag_set(const char *name, const ProllyHash *expected,
			   const ProllyHash *tag)
{
	ProllyHash	cur;
	bool		exists;
	ProllyTagInfo info;
	StringInfoData key;
	StringInfoData v;

	refs_init();
	lock_refs();
	exists = tag_entry(name, &cur, NULL);
	if (expected != NULL &&
		(expected->len == 0 ? exists : !(exists && same_hash(&cur, expected))))
		return false;
	if (tag == NULL && !exists)
		return false;
	if (tag != NULL)
	{
		ProllyHash	id;
		ProllyCommitInfo ci;

		/*
		 * A new tag's name follows a branch's rules, and isn't a commit's
		 * id, which it would hide where refs are read.
		 */
		if (!exists &&
			(!branch_ops()->name_ok(name) ||
			 (parse_commit_id(name, &id) && copies_commit_get(&id, &ci))))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("invalid tag name \"%s\"", name),
					 errdetail("A tag's name follows a branch's rules, and isn't a commit's id.")));
		if (!copies_tag_get(tag, &info))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tag object %s does not exist", prolly_hash_hex(tag))));
	}
	initStringInfo(&key);
	initStringInfo(&v);
	tag_key(name, &key);
	if (tag)
	{
		put_hash(&v, tag);
		put_hash(&v, &info.commit);
	}
	refs_put(key.data, key.len, tag ? &v : NULL);
	/* (a tag deleted, made or moved is no one's until its maker is told) */
	maker_put(TAG_TAG_CREATOR, name, InvalidOid);
	pfree(key.data);
	pfree(v.data);
	return true;
}

typedef struct TagVisit
{
	ProllyTagVisit visit;
	void	   *arg;
} TagVisit;

static void
visit_tag_entry(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	TagVisit   *tv = arg;
	char	   *name = pnstrdup((const char *) k->data + 1, k->len - 1);
	const char *p = (const char *) v->data;
	const char *end = p + v->len;
	ProllyHash	tag;
	ProllyHash	commit;

	p = get_hash(p, end, &tag);
	(void) get_hash(p, end, &commit);
	tv->visit(name, &tag, &commit, tv->arg);
	pfree(name);
}

static void
copies_tags(ProllyTagVisit visit, void *arg)
{
	TagVisit	tv = {visit, arg};
	char		tag = TAG_TAG;

	refs_init();
	visit_prefix(&tag, 1, visit_tag_entry, &tv);
}

/* ------------------------------------------------------------------------
 * Pins: TAG_PIN and a pin's name -> the commit it keeps, then its holder's
 * record (the rest of the value, its own bytes).
 * ------------------------------------------------------------------------
 */

static void
pin_key(const char *name, StringInfo key)
{
	appendStringInfoChar(key, TAG_PIN);
	appendStringInfoString(key, name);
}

/*
 * Pin commit under name with info beside it (commit NULL: unpin it).  The
 * refs are held first, as for any change of them that isn't a branch's.
 */
static void
copies_pin_set(const char *name, const ProllyHash *commit,
			   const ProllyHash *keep, int nkeep, const ProllyValue *info)
{
	StringInfoData key;
	StringInfoData v;

	refs_init();
	lock_refs();
	if (commit)
	{
		ProllyCommitInfo ci;

		if (!copies_commit_get(commit, &ci))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("commit %s does not exist", prolly_hash_hex(commit))));
	}
	initStringInfo(&key);
	initStringInfo(&v);
	pin_key(name, &key);
	if (commit)
	{
		put_hash(&v, commit);
		prolly_put_varint(&v, nkeep);
		for (int i = 0; i < nkeep; i++)
		{
			if (!PROLLY_CALL(env.chunk_store, has, env.chunks, &keep[i]))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("tree %s does not exist", prolly_hash_hex(&keep[i]))));
			put_hash(&v, &keep[i]);
		}
		if (info)
			appendBinaryStringInfo(&v, (const char *) info->data, info->len);
	}
	refs_put(key.data, key.len, commit ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
}

static bool
copies_pin_get(const char *name, ProllyHash *commit, StringInfo info)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;

	refs_init();
	initStringInfo(&key);
	initStringInfo(&v);
	pin_key(name, &key);
	found = refs_get(key.data, key.len, &v);
	if (found)
	{
		const char *end = v.data + v.len;
		const char *p = get_hash(v.data, end, commit);
		uint64		n = prolly_get_varint(&p, end);
		ProllyHash	h;

		for (uint64 i = 0; i < n && p < end; i++)
			p = get_hash(p, end, &h);
		if (info)
			appendBinaryStringInfo(info, p, v.data + v.len - p);
	}
	pfree(key.data);
	pfree(v.data);
	return found;
}

typedef struct PinVisit
{
	ProllyPinVisit visit;
	void	   *arg;
} PinVisit;

static void
visit_pin_entry(const ProllyKey *k, const ProllyValue *v, void *arg)
{
	PinVisit   *pv = arg;
	char	   *name = pnstrdup((const char *) k->data + 1, k->len - 1);
	const char *end = (const char *) v->data + v->len;
	ProllyHash	commit;
	const char *p = get_hash((const char *) v->data, end, &commit);
	uint64		n = prolly_get_varint(&p, end);
	ProllyHash	h;
	ProllyValue info;

	for (uint64 i = 0; i < n && p < end; i++)
		p = get_hash(p, end, &h);
	info.data = (const uint8 *) p;
	info.len = (uint32) (end - p);

	pv->visit(name, &commit, &info, pv->arg);
	pfree(name);
}

static void
copies_pins(ProllyPinVisit visit, void *arg)
{
	PinVisit	pv = {visit, arg};
	char		tag = TAG_PIN;

	refs_init();
	visit_prefix(&tag, 1, visit_pin_entry, &pv);
}

/* ------------------------------------------------------------------------
 * Rebases: TAG_REBASE and the branch rebasing on -> its rebase state.
 * ------------------------------------------------------------------------
 */

static void
rebase_key(StringInfo key, const char *branch)
{
	appendStringInfoChar(key, TAG_REBASE);
	appendStringInfoString(key, branch);
}

static void
put_short_text(StringInfo out, const char *s)
{
	prolly_put_varint(out, strlen(s));
	appendStringInfoString(out, s);
}

static bool
copies_rebase_state(ProllyRebaseState *state)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;

	refs_init();
	initStringInfo(&key);
	initStringInfo(&v);
	rebase_key(&key, branch_ops()->current());
	found = refs_get(key.data, key.len, &v);
	if (found && !rebase_decode(v.data, v.data + v.len, state))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupt prolly rebase state")));
	pfree(key.data);
	pfree(v.data);
	return found;
}

static void
copies_set_rebase_state(const ProllyRebaseState *state)
{
	StringInfoData key;
	StringInfoData v;

	refs_init();
	initStringInfo(&key);
	initStringInfo(&v);
	rebase_key(&key, branch_ops()->current());
	if (state)
	{
		put_short_text(&v, state->branch);
		put_hash(&v, &state->head);
		put_hash(&v, &state->onto);
		appendStringInfoChar(&v, state->keep_empty ? 1 : 0);
		put_short_text(&v, state->last_step ? state->last_step : "");
		prolly_put_varint(&v, state->nsteps);
		for (int i = 0; i < state->nsteps; i++)
		{
			const ProllyRebaseStep *s = &state->steps[i];

			put_short_text(&v, s->order);
			put_short_text(&v, s->action);
			put_hash(&v, &s->commit);
			put_short_text(&v, s->message);
		}
	}
	refs_put(key.data, key.len, state ? &v : NULL);
	pfree(key.data);
	pfree(v.data);
}

/* ------------------------------------------------------------------------
 * Stashes: TAG_STASH and a list's name, its entries newest first, each its
 * commit's hash, then its branch's name (a varint length, then the bytes),
 * then the maker record of the role that pushed it (MAKER_RECORD_LEN).
 * ------------------------------------------------------------------------
 */

typedef struct StashEntry
{
	ProllyHash	stash;
	char	   *branch;
	MakerRecord maker;
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
		if (blen + MAKER_RECORD_LEN > (uint64) (end - p))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupt prolly stash list")));
		e->branch = pnstrdup(p, blen);
		p += blen;
		memcpy(&e->maker.role, p, sizeof(Oid));
		memcpy(&e->maker.id, p + sizeof(Oid), sizeof(Oid));
		p += MAKER_RECORD_LEN;
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
		appendBinaryStringInfo(&v, &e->maker.role, sizeof(Oid));
		appendBinaryStringInfo(&v, &e->maker.id, sizeof(Oid));
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
	{
		StringInfoData identity;

		initStringInfo(&identity);
		identify_working(InvalidOid, &identity, NULL);
		write_commit(&root->hash, &head, 1, prolly_commit_author(),
					 psprintf("WIP on %s", branch), &identity, stash);
	}
	/* (made of the working set, its relations: a pop tells them apart) */
	{
		StringInfoData lines;

		initStringInfo(&lines);
		table_names(&lines);
		note_relations(stash, lines.data);
		pfree(lines.data);
	}
	e->stash = *stash;
	e->branch = pstrdup(branch);
	e->maker.role = e->maker.id = InvalidOid;
	maker_change(RefsStashClassId, &e->maker, GetUserId());
	stash_set(name, lcons(e, entries));
}

/* The role that pushed name's nth stash; false if it has none. */
static bool
copies_stash_maker(const char *name, int n, Oid *role)
{
	List	   *entries;

	refs_init();
	lock_refs();
	entries = stash_get(name);
	if (n < 0 || n >= list_length(entries))
		return false;
	*role = ((StashEntry *) list_nth(entries, n))->maker.role;
	return true;
}

static void
copies_stash_set_maker(const char *name, int n, Oid role)
{
	List	   *entries;

	refs_init();
	lock_refs();
	entries = stash_get(name);
	if (n < 0 || n >= list_length(entries))
		return;
	maker_change(RefsStashClassId,
				 &((StashEntry *) list_nth(entries, n))->maker, role);
	stash_set(name, entries);
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
	StashEntry *e;

	lock_refs();
	entries = stash_get(name);
	if (n < 0 || n >= list_length(entries))
		return false;
	e = list_nth(entries, n);
	maker_change(RefsStashClassId, &e->maker, InvalidOid);
	stash_set(name, list_delete_nth_cell(entries, n));
	return true;
}

static void
copies_stash_clear(const char *name)
{
	List	   *entries;
	ListCell   *lc;

	lock_refs();
	entries = stash_get(name);
	foreach(lc, entries)
		maker_change(RefsStashClassId, &((StashEntry *) lfirst(lc))->maker,
					 InvalidOid);
	if (entries != NIL)
		stash_set(name, NIL);
}

/*
 * The object of kind classId whose maker record has id: a branch or tag
 * by its 'A' or 'G' entries, a stash by the lists' entries.
 */
static bool
copies_made_object(Oid classId, Oid id, StringInfo name, int *n)
{
	ProllyNodeRef root;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	char		tag;
	ProllyKey	from = {(const uint8 *) &tag, 1};
	bool		found = false;

	if (classId == RefsBranchClassId)
		tag = TAG_CREATOR;
	else if (classId == RefsTagClassId)
		tag = TAG_TAG_CREATOR;
	else if (classId == RefsStashClassId)
		tag = TAG_STASH;
	else
		return false;
	refs_init();
	if (!refs_root(&root))
		return false;
	c = PROLLY_CALL(tree, cursor_open, &env, &root, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, &from, ForwardScanDirection);
	while (!found && PROLLY_CALL(tree, cursor_next, c, &k, &v) &&
		   k.len >= 1 && k.data[0] == tag)
	{
		if (tag == TAG_STASH)
		{
			int			i = 0;
			ListCell   *lc;

			foreach(lc, stash_list((const char *) v.data,
								   (const char *) v.data + v.len))
			{
				if (((StashEntry *) lfirst(lc))->maker.id == id)
				{
					*n = i;
					found = true;
					break;
				}
				i++;
			}
		}
		else if (v.len == MAKER_RECORD_LEN)
		{
			Oid			rid;

			memcpy(&rid, v.data + sizeof(Oid), sizeof(Oid));
			found = rid == id;
		}
		if (found)
			appendBinaryStringInfo(name, (const char *) k.data + 1, k.len - 1);
	}
	PROLLY_CALL(tree, cursor_close, c);
	return found;
}

/* ------------------------------------------------------------------------
 * Other components' entries
 * ------------------------------------------------------------------------
 */

static void
side_key(StringInfo out, char space, const ProllyKey *key)
{
	appendStringInfoChar(out, TAG_SIDE);
	appendStringInfoChar(out, space);
	if (key)
		appendBinaryStringInfo(out, (const char *) key->data, key->len);
}

static bool
copies_side_get(char space, const ProllyKey *key, StringInfo value)
{
	StringInfoData k;
	bool		found;

	initStringInfo(&k);
	side_key(&k, space, key);
	found = refs_get(k.data, k.len, value);
	pfree(k.data);
	return found;
}

static void
copies_side_put(char space, const ProllyKey *key, const ProllyValue *value)
{
	StringInfoData k;
	StringInfoData v;

	initStringInfo(&k);
	side_key(&k, space, key);
	if (value)
	{
		v.data = (char *) value->data;
		v.len = value->len;
		v.maxlen = value->len + 1;
		v.cursor = 0;
	}
	refs_put(k.data, k.len, value ? &v : NULL);
	pfree(k.data);
}

typedef bool (*SideVisit) (const ProllyKey *key, const ProllyValue *value,
						   void *arg);

/* A side scan of the refs tree root, its nodes read through renv. */
static void
scan_side(const ProllyNodeRef *rootp, const ProllyTreeEnv *renv, char space,
		  const ProllyKey *from, ScanDirection direction, SideVisit visit,
		  void *arg)
{
	ProllyNodeRef root = *rootp;
	ProllyTreeCursor *c;
	StringInfoData start;
	char		prefix[2] = {TAG_SIDE, space};
	ProllyKey	k;
	ProllyValue v;

	initStringInfo(&start);
	side_key(&start, space, from);
	/*
	 * (backward from none: from the space's end, past every key of it: its
	 * prefix then more 0xff bytes than any key has, whatever the space's
	 * byte is)
	 */
	if (from == NULL && ScanDirectionIsBackward(direction))
	{
		Size		max = PROLLY_CALL(tree, key_max, env.chunk_format,
									  env.hash);

		for (Size i = 0; i <= max; i++)
			appendStringInfoChar(&start, (char) 0xff);
	}
	c = PROLLY_CALL(tree, cursor_open, renv, &root, CurrentMemoryContext);
	{
		ProllyKey	seek = {(const uint8 *) start.data, start.len};

		PROLLY_CALL(tree, cursor_seek, c, &seek, direction);
	}
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		ProllyKey	own;

		if (k.len < 2 || memcmp(k.data, prefix, 2) != 0)
			break;
		own.data = k.data + 2;
		own.len = k.len - 2;
		if (!visit(&own, &v, arg))
			break;
	}
	PROLLY_CALL(tree, cursor_close, c);
	pfree(start.data);
}

static void
copies_side_scan(char space, const ProllyKey *from, ScanDirection direction,
				 SideVisit visit, void *arg)
{
	ProllyNodeRef root;

	if (refs_root(&root))
		scan_side(&root, &env, space, from, direction, visit, arg);
}

/* A store of nothing but a reader's chunks: what side_scan_at() reads. */
typedef struct ReaderStore
{
	ProllyChunkRead read;
	void	   *arg;
} ReaderStore;

static bool
reader_get(ProllyChunkStore *store, const ProllyHash *hash, StringInfo out)
{
	ReaderStore *rs = (ReaderStore *) store;

	return rs->read(rs->arg, hash, out);
}

static bool
reader_has(ProllyChunkStore *store, const ProllyHash *hash)
{
	StringInfoData data;
	bool		found;

	initStringInfo(&data);
	found = reader_get(store, hash, &data);
	pfree(data.data);
	return found;
}

static const ProllyChunkStoreRoutine reader_store = {
	.info = {
		.iface = PROLLY_IFACE_CHUNK_STORE,
		.name = "reader",
		.description = "the chunks a reader gives",
	},
	.get = reader_get,
	.has = reader_has,
};

static void
first_child(const ProllyHash *ref, void *arg)
{
	ProllyHash *first = arg;

	if (first->len == 0)
		*first = *ref;
}

/*
 * Is root a refs tree of this format, as the reader has it?  Its leftmost
 * path read, each node one of the format's, down to the first key, the
 * format's entry.  (A store's root may be something else in a test of the
 * store alone.)
 */
static bool
reader_refs_root(ReaderStore *rs, const ProllyHash *root)
{
	ProllyHash	at = *root;
	StringInfoData data;
	bool		ok = false;

	initStringInfo(&data);
	for (int depth = 0; depth < 64; depth++)
	{
		ProllyChunkNode node;

		resetStringInfo(&data);
		if (!rs->read(rs->arg, &at, &data) ||
			!PROLLY_CALL(env.chunk_format, open, (const uint8 *) data.data,
						 data.len, &node) || node.nitems == 0)
			break;
		if (node.level == 0)
		{
			ProllyKey	k;
			ProllyValue v;
			uint8		flags;

			PROLLY_CALL(env.chunk_format, item_get, &node, 0, &k, &v, &flags);
			ok = k.len == 1 && k.data[0] == TAG_FORMAT &&
				v.len == strlen(REFS_FORMAT) &&
				memcmp(v.data, REFS_FORMAT, v.len) == 0;
			break;
		}
		memset(&at, 0, sizeof(at));
		prolly_chunk_references(data.data, data.len, first_child, &at);
		if (at.len == 0)
			break;
	}
	pfree(data.data);
	return ok;
}

static void
copies_side_scan_at(const ProllyHash *root, ProllyChunkRead read,
					void *read_arg, char space, const ProllyKey *from,
					ScanDirection direction, SideVisit visit, void *arg)
{
	ReaderStore rs = {read, read_arg};
	ProllyTreeEnv renv;
	ProllyNodeRef r;

	refs_init();
	if (root->len == 0 || !reader_refs_root(&rs, root))
		return;
	renv = env;
	renv.chunk_store = &reader_store;
	renv.chunks = (ProllyChunkStore *) &rs;
	renv.counters = NULL;
	r.hash = *root;
	scan_side(&r, &renv, space, from, direction, visit, arg);
}

/* ------------------------------------------------------------------------
 * Head logs
 *
 * Each branch's head log is its head's moves in order, each with its date
 * (when it was made, never before the move before it) and the commit it
 * moved to: "the commit branch B was at at time T" is one seek, whatever
 * dates the commits were given.  A remote-tracking branch has one too,
 * moved as fetches move it.
 *
 * A log is a tree of its own (keys by memcmp: each move's date, 8 bytes
 * big-endian with the sign bit flipped, then its number in the log, 8
 * bytes big-endian; its value the commit, its hash's length then the
 * hash's bytes complemented, so that it names no chunk).  Moves come in
 * date order, so each goes at the tree's right edge.  A branch's log is
 * in its mover entry, which every move writes anyway: the tree's root,
 * and its latest moves, HEAD_LOG_TAIL of them at most, which then go into
 * the tree together.  So a move writes no more of the refs' paths, and
 * the tree's right edge once every HEAD_LOG_TAIL moves.  A
 * remote-tracking branch's log has an entry of its own, written as a
 * fetch or push moves it.
 * ------------------------------------------------------------------------
 */

#define HEAD_LOG_TAIL	16

typedef struct LogState
{
	ProllyHash	root;			/* len 0: no tree yet */
	int64		count;			/* moves in the tree */
	TimestampTz last;			/* the last move's date (in the tree) */
} LogState;

static void
put_log_id(StringInfo out, const ProllyHash *id)
{
	appendStringInfoChar(out, (char) id->len);
	for (int i = 0; i < id->len; i++)
		appendStringInfoChar(out, (char) ~id->bytes[i]);
}

static const char *
get_log_id(const char *p, const char *end, ProllyHash *id)
{
	if (p >= end || (uint8) *p > PROLLY_HASH_MAX_LEN ||
		end - p - 1 < (uint8) *p)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a prolly head log is corrupt")));
	memset(id, 0, sizeof(*id));
	id->len = (uint8) *p;
	for (int i = 0; i < id->len; i++)
		id->bytes[i] = (uint8) ~p[1 + i];
	return p + 1 + id->len;
}

static void
put_log_date(StringInfo out, TimestampTz at)
{
	put_uint64(out, ((uint64) at) ^ ((uint64) 1 << 63));
}

static TimestampTz
get_log_date(const char *p)
{
	uint64		v = 0;

	for (int i = 0; i < 8; i++)
		v = (v << 8) | (uint8) p[i];
	return (TimestampTz) (v ^ ((uint64) 1 << 63));
}

static uint64
get_be64_at(const char *p)
{
	uint64		v = 0;

	for (int i = 0; i < 8; i++)
		v = (v << 8) | (uint8) p[i];
	return v;
}

static const char *get_log_state(const char *p, const char *end,
								 LogState *ls);

/* A remote-tracking branch's log's entry. */
static void
log_key(StringInfo key, bool tracking, const char *name, const char *branch)
{
	Assert(tracking);
	appendStringInfoChar(key, TAG_TRACKING_LOG);
	appendStringInfoString(key, name);
	appendStringInfoChar(key, '\0');
	if (branch)
		appendStringInfoString(key, branch);
}

static void
log_state(const StringInfoData *key, LogState *ls)
{
	StringInfoData v;

	memset(ls, 0, sizeof(*ls));
	ls->last = DT_NOBEGIN;
	initStringInfo(&v);
	if (refs_get(key->data, key->len, &v))
		(void) get_log_state(v.data, v.data + v.len, ls);
	pfree(v.data);
}

/* A move of a log: its number, date and commit. */
typedef struct LogMove
{
	int64		seq;
	TimestampTz at;
	ProllyHash	commit;
} LogMove;

/* Add moves (in order, numbered from the log's count) to its tree. */
static void
tree_append(LogState *ls, const LogMove *moves, int n)
{
	ProllyEdit *edits = palloc_array(ProllyEdit, Max(n, 1));
	ProllyNodeRef root;
	ProllyNodeRef next;

	for (int i = 0; i < n; i++)
	{
		StringInfoData k;
		StringInfoData val;

		initStringInfo(&k);
		put_log_date(&k, moves[i].at);
		put_uint64(&k, (uint64) moves[i].seq);
		initStringInfo(&val);
		put_log_id(&val, &moves[i].commit);
		edits[i].key.data = (const uint8 *) k.data;
		edits[i].key.len = k.len;
		edits[i].value.data = (const uint8 *) val.data;
		edits[i].value.len = val.len;
		edits[i].is_delete = false;
	}
	if (ls->root.len == 0)
	{
		ProllyTreeBuilder *b = PROLLY_CALL(tree, build_begin, &log_env,
										   CurrentMemoryContext);

		PROLLY_CALL(tree, build_finish, b, &root);
	}
	else
		ref_of(&ls->root, &root);
	if (n > 0)
		PROLLY_CALL(tree, apply, &log_env, &root, edits, n, &next);
	else
		next = root;
	ls->root = next.hash;
	ls->count += n;
	if (n > 0)
		ls->last = moves[n - 1].at;
	pfree(edits);
}

/* A log's state, as its entry (or a mover entry) holds it. */
static void
put_log_state(StringInfo v, const LogState *ls)
{
	put_hash(v, &ls->root);
	put_uint64(v, (uint64) ls->count);
	put_uint64(v, (uint64) ls->last);
}

static const char *
get_log_state(const char *p, const char *end, LogState *ls)
{
	p = get_hash(p, end, &ls->root);
	if (end - p < 16)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a prolly head log is corrupt")));
	ls->count = (int64) get_be64_at(p);
	ls->last = (TimestampTz) get_be64_at(p + 8);
	return p + 16;
}

/*
 * The head log a branch's mover entry holds (value v): its tree's state
 * (*ls) and its tail, at most HEAD_LOG_TAIL moves, the last of them to
 * head (the branch's head when the entry was written, which it doesn't
 * repeat).  0, the log empty, if it holds none.
 */
static int
tail_moves(const StringInfoData *v, const ProllyHash *head, LogMove *moves,
		   LogState *ls)
{
	const char *p;
	const char *end = v->data + v->len;
	int			n;

	memset(ls, 0, sizeof(*ls));
	ls->last = DT_NOBEGIN;
	if (v->len <= sizeof(FullTransactionId))
		return 0;
	p = v->data + sizeof(FullTransactionId);
	n = (uint8) *p++;
	if (n & TAIL_TREE)
		p = get_log_state(p, end, ls);
	n &= ~TAIL_TREE;
	if (n > HEAD_LOG_TAIL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a prolly head log is corrupt")));
	for (int i = 0; i < n; i++)
	{
		if (end - p < 8)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a prolly head log is corrupt")));
		moves[i].at = (TimestampTz) get_be64_at(p);
		p += 8;
		if (i < n - 1)
			p = get_log_id(p, end, &moves[i].commit);
		else
			moves[i].commit = *head;
	}
	return n;
}

/*
 * Branch name's head log's tail and state, its mover entry's; head, if
 * not NULL, the head the entry was written at (else the branch's now).
 */
static bool
branch_tail(const char *name, const ProllyHash *head, LogMove *moves, int *n,
			LogState *ls)
{
	StringInfoData key;
	StringInfoData v;
	bool		found;
	ProllyHash	now;

	if (head == NULL)
	{
		(void) branch_head(name, &now);
		head = &now;
	}
	initStringInfo(&key);
	initStringInfo(&v);
	appendStringInfoChar(&key, TAG_MOVER);
	appendStringInfoString(&key, name);
	found = refs_get(key.data, key.len, &v);
	*n = 0;
	if (found)
		*n = tail_moves(&v, head, moves, ls);
	else
		(void) tail_moves(&v, head, moves, ls);
	pfree(key.data);
	pfree(v.data);
	return found;
}

/*
 * Branch name's head moved from was (len 0: none) to head: the move is
 * added to the log's tail, which goes into the log's tree once full; the
 * log (the tree's state, then the tail left) is put in mover, its mover
 * entry's value, after the transaction that moved it.  The mover entry is
 * written by every move anyway, so the log costs a move no more of the
 * refs' paths; and the tail's last move, to the head, doesn't repeat it.
 */
static void
head_log_note(const char *name, const ProllyHash *was, const ProllyHash *head,
			  StringInfo mover)
{
	LogMove		tail[HEAD_LOG_TAIL + 1];
	int			n;
	LogState	ls;
	TimestampTz at = GetCurrentTimestamp();

	(void) branch_tail(name, was, tail, &n, &ls);
	/* (dates only rise along a log, whatever the clock does) */
	if (n > 0)
		at = Max(at, tail[n - 1].at);
	at = Max(at, ls.last);
	tail[n].at = at;
	tail[n].commit = *head;
	n++;
	if (n > HEAD_LOG_TAIL)
	{
		for (int i = 0; i < n; i++)
			tail[i].seq = ls.count + i;
		tree_append(&ls, tail, n);
		n = 0;
	}
	appendStringInfoChar(mover, (char) (n | (ls.root.len > 0 ? TAIL_TREE : 0)));
	if (ls.root.len > 0)
		put_log_state(mover, &ls);
	for (int i = 0; i < n; i++)
	{
		put_uint64(mover, (uint64) tail[i].at);
		if (i < n - 1)
			put_log_id(mover, &tail[i].commit);
	}
}

/* A remote-tracking branch moved (commit NULL: removed): logged at once. */
static void
tracking_log_note(const char *remote, const char *branch,
				  const ProllyHash *commit)
{
	StringInfoData key;
	LogState	ls;
	LogMove		m;

	initStringInfo(&key);
	log_key(&key, true, remote, branch);
	if (commit == NULL)
	{
		refs_put(key.data, key.len, NULL);
		pfree(key.data);
		return;
	}
	log_state(&key, &ls);
	m.seq = ls.count;
	m.at = Max(GetCurrentTimestamp(), ls.last);
	m.commit = *commit;
	tree_append(&ls, &m, 1);
	{
		StringInfoData v;

		initStringInfo(&v);
		put_log_state(&v, &ls);
		refs_put(key.data, key.len, &v);
		pfree(v.data);
	}
	pfree(key.data);
}

/* A log's moves, its tree's then its tail's: all of them, in order. */
static List *
log_moves(const char *name, const char *branch, TimestampTz from,
		  TimestampTz to)
{
	StringInfoData key;
	LogState	ls;
	List	   *moves = NIL;
	LogMove		tail[HEAD_LOG_TAIL + 1];
	int			n = 0;

	initStringInfo(&key);
	log_key(&key, true, name, branch);
	if (branch == NULL)
		(void) branch_tail(name, NULL, tail, &n, &ls);
	else
		log_state(&key, &ls);
	if (ls.root.len > 0 && from <= to)
	{
		ProllyNodeRef root;
		ProllyTreeCursor *c;
		StringInfoData seek;
		ProllyKey	sk;
		ProllyKey	k;
		ProllyValue v;

		ref_of(&ls.root, &root);
		initStringInfo(&seek);
		put_log_date(&seek, from);
		sk.data = (const uint8 *) seek.data;
		sk.len = seek.len;
		c = PROLLY_CALL(tree, cursor_open, &log_env, &root,
						CurrentMemoryContext);
		PROLLY_CALL(tree, cursor_seek, c, &sk, ForwardScanDirection);
		while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
		{
			LogMove    *m;

			if (k.len != 16)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("a prolly head log is corrupt")));
			if (get_log_date((const char *) k.data) > to)
				break;
			m = palloc(sizeof(LogMove));
			m->at = get_log_date((const char *) k.data);
			m->seq = (int64) get_be64_at((const char *) k.data + 8);
			(void) get_log_id((const char *) v.data,
							  (const char *) v.data + v.len, &m->commit);
			moves = lappend(moves, m);
		}
		PROLLY_CALL(tree, cursor_close, c);
		pfree(seek.data);
	}
	for (int i = 0; i < n; i++)
	{
		LogMove    *m;

		if (tail[i].at < from || tail[i].at > to)
			continue;
		m = palloc(sizeof(LogMove));
		*m = tail[i];
		m->seq = ls.count + i;
		moves = lappend(moves, m);
	}
	pfree(key.data);
	return moves;
}

/*
 * The move of a log last made at or before at: its tail's newest such,
 * else a seek of its tree.
 */
static bool
copies_head_at(const char *name, const char *branch, TimestampTz at,
			   ProllyHeadMove *move)
{
	StringInfoData key;
	LogState	ls;
	LogMove		tail[HEAD_LOG_TAIL + 1];
	int			n = 0;
	bool		found = false;

	refs_init();
	initStringInfo(&key);
	log_key(&key, true, name, branch);
	if (branch == NULL)
		(void) branch_tail(name, NULL, tail, &n, &ls);
	else
		log_state(&key, &ls);
	for (int i = n - 1; i >= 0 && !found; i--)
		if (tail[i].at <= at)
		{
			move->seq = ls.count + i;
			move->moved_at = tail[i].at;
			move->commit = tail[i].commit;
			found = true;
		}
	if (!found && ls.root.len > 0)
	{
		ProllyNodeRef root;
		ProllyTreeCursor *c;
		StringInfoData seek;
		ProllyKey	sk;
		ProllyKey	k;
		ProllyValue v;

		ref_of(&ls.root, &root);
		initStringInfo(&seek);
		put_log_date(&seek, at);
		put_uint64(&seek, PG_UINT64_MAX);
		sk.data = (const uint8 *) seek.data;
		sk.len = seek.len;
		c = PROLLY_CALL(tree, cursor_open, &log_env, &root,
						CurrentMemoryContext);
		PROLLY_CALL(tree, cursor_seek, c, &sk, BackwardScanDirection);
		if (PROLLY_CALL(tree, cursor_next, c, &k, &v) && k.len == 16)
		{
			move->moved_at = get_log_date((const char *) k.data);
			move->seq = (int64) get_be64_at((const char *) k.data + 8);
			(void) get_log_id((const char *) v.data,
							  (const char *) v.data + v.len, &move->commit);
			found = true;
		}
		PROLLY_CALL(tree, cursor_close, c);
		pfree(seek.data);
	}
	pfree(key.data);
	return found;
}

static void
copies_head_moves(const char *name, const char *branch, TimestampTz from,
				  TimestampTz to, ScanDirection direction,
				  ProllyHeadMoveVisit visit, void *arg)
{
	List	   *moves;

	refs_init();
	moves = log_moves(name, branch, from, to);
	for (int i = 0; i < list_length(moves); i++)
	{
		LogMove    *m = list_nth(moves, ScanDirectionIsBackward(direction) ?
								 list_length(moves) - 1 - i : i);
		ProllyHeadMove hm = {m->seq, m->at, m->commit};

		if (!visit(&hm, arg))
			break;
	}
}

const ProllyRefsRoutine prolly_refs_copies = {
	.info = {
		.iface = PROLLY_IFACE_REFS,
		.impl_id = 2,
		.format_version = 3,
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
	.working_identity = copies_working_identity,
	.anchor_identity = copies_anchor_identity,
	.branches = copies_branches,
	.branches_named = copies_branches_named,
	.working_set = copies_working_set,
	.read_working_set = copies_read_working_set,
	.read_definitions = copies_read_definitions,
	.working_changes = copies_working_changes,
	.publish_definitions = copies_publish_definitions,
	.fold_own = copies_fold_own,
	.pending_full_folds = copies_pending_full_folds,
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
	.visit_violations = copies_visit_violations,
	.add_violations = copies_add_violations,
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
	.tag_write = copies_tag_write,
	.tag_get = copies_tag_get,
	.tag_lookup = copies_tag_lookup,
	.tag_set = copies_tag_set,
	.tags = copies_tags,
	.pin_set = copies_pin_set,
	.pin_get = copies_pin_get,
	.pins = copies_pins,
	.rebase_state = copies_rebase_state,
	.set_rebase_state = copies_set_rebase_state,
	.commit_root = copies_commit_root,
	.kept_commits = copies_kept_commits,
	.kept_holders = copies_kept_holders,
	.recorded_relid = copies_recorded_relid,
	.branch_creator = copies_branch_creator,
	.branch_set_creator = copies_branch_set_creator,
	.tag_creator = copies_tag_creator,
	.tag_set_creator = copies_tag_set_creator,
	.stash_maker = copies_stash_maker,
	.commit_relations = copies_commit_relations,
	.made_object = copies_made_object,
	.stash_set_maker = copies_stash_set_maker,
	.last_fold = copies_last_fold,
	.forget_records = copies_forget_records,
	.check_copies_read = copies_check_copies_read,
	.branches_folded = copies_branches_folded,
	.head_at = copies_head_at,
	.head_moves = copies_head_moves,
	.side_get = copies_side_get,
	.side_put = copies_side_put,
	.side_scan = copies_side_scan,
	.side_scan_at = copies_side_scan_at,
};
