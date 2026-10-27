/*-------------------------------------------------------------------------
 *
 * window_archive.c
 *	  History window implementation "archive": history past a window
 *	  severed at a horizon kept in the refs.
 *
 * The horizon is entries of the refs (their side_put(), space 'h'):
 *
 *	'e' parent boundary	-> a boundary edge: the boundary commit keeps no
 *						   parent (each an id: its hash's length, a byte,
 *						   then the hash); the fragment's number (8 bytes
 *						   big-endian)
 *	'f' number			-> a fragment (8 bytes big-endian; see
 *						   fragment_encode())
 *	'n'					-> the next fragment's number
 *
 * None names a chunk: a collection asks edges() which parents not to
 * follow from which commits, and keeps nothing for a fragment.
 *
 * What is kept.  Every commit the refs name is kept (a branch's head, a
 * tag's, a remote-tracking branch's, a stash's, a merge's or rebase's in
 * progress, a pin's, a kept commit's).  From each branch head (but a
 * session's detached one, which is a ref only) a walk keeps the commits
 * its window does (dated at or after the cut), and every commit on the
 * way to its merge base with each other branch head (one at least as high
 * as the lowest of those bases): merging two live branches never needs a
 * severed commit.  The commits kept are the window; an edge from one to a
 * parent outside it is a boundary edge, and the parent a top of the
 * fragment, which is every commit the tops reach outside the window and
 * the horizon there was already.
 *
 * A fragment's identity (its root) is the root hash of a tree of its
 * commits' index entries, keyed by date then id, built as any tree is:
 * the same commits give the same root wherever they are severed.  Its
 * digest is a hash over the sorted hashes of the chunks its commits reach
 * (their own trees, their roots' and dependencies'), which repair
 * recomputes, from the same tops down to the same stops (the commits
 * outside the fragment its commits name), once the chunks are back.
 *
 * Severing changes no commit: hashes stay, and the boundary commits still
 * name their parents.  Readers that list history (dolt_log, history
 * tables) stop at the horizon, as a shallow clone's do; one that needs a
 * commit past it (dolt_at, a diff, a merge's base, a cherry-pick) finds
 * it missing once a collection freed it, and is told it is past the
 * horizon and how to repair it (prolly_window_check_missing()).
 *
 * A sever, a repair and a collection exclude one another: a collection
 * holds the horizon's lock shared, and those that change the horizon
 * exclusively, so a collection never frees by a horizon a repair has just
 * let go of.  A collection in the transaction that changed the horizon is
 * refused: the horizon might yet roll back, and what it freed with it.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/window_archive.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "access/xlog.h"
#include "access/prolly_datum.h"
#include "access/xact.h"
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

#define SPACE_HORIZON	'h'
#define KEY_EDGE		'e'
#define KEY_FRAGMENT	'f'
#define KEY_NEXT		'n'

#define FRAGMENT_VERSION	1
#define FRAGMENT_DELETED	0x01

/* The horizon's lock: an object lock in the database, of this class. */
#define HORIZON_LOCK_CLASSID	((Oid) 0x50524848)

static void check_may_sever(const char *what);
static void sever_cut(ProllyWindowAction action, const TimestampTz *before,
					  TimestampTz size_cut, ProllyWindowResult *result);
static char *source_url(const char *source);

/* ------------------------------------------------------------------------
 * Components and encodings
 * ------------------------------------------------------------------------
 */

static ProllyDatabaseComponents dc;
static bool dc_ready = false;
static ProllyTreeEnv tree_env;
static const ProllyTreeRoutine *tree;

static void window_subxact(SubXactEvent event, SubTransactionId mySubid,
						   SubTransactionId parentSubid, void *arg);

static void
window_init(void)
{
	if (dc_ready)
		return;
	RegisterSubXactCallback(window_subxact, NULL);
	prolly_database_components(&dc);
	tree = prolly_tree_lookup("cow");
	memset(&tree_env, 0, sizeof(tree_env));
	tree_env.chunk_format = prolly_chunkformat_lookup("basic");
	tree_env.chunker = prolly_chunker_lookup("bytes");
	tree_env.hash = prolly_content_hash();
	tree_env.chunk_store = prolly_chunk_store();
	tree_env.chunks = PROLLY_CALL(tree_env.chunk_store, open, MyDatabaseId,
								  MyDatabaseTableSpace);
	tree_env.compare = prolly_bytes_compare;
	dc_ready = true;
}

static void
put_be64(StringInfo out, uint64 v)
{
	for (int i = 7; i >= 0; i--)
		appendStringInfoChar(out, (char) (v >> (i * 8)));
}

static uint64
get_be64(const char *p)
{
	uint64		v = 0;

	for (int i = 0; i < 8; i++)
		v = (v << 8) | (uint8) p[i];
	return v;
}

static void
put_id(StringInfo out, const ProllyHash *id)
{
	appendStringInfoChar(out, (char) id->len);
	appendBinaryStringInfo(out, (const char *) id->bytes, id->len);
}

/*
 * An id in a horizon entry's value: its bytes complemented, so that no
 * bytes of the value spell a reference as a value naming a chunk does.
 */
static void
put_value_id(StringInfo out, const ProllyHash *id)
{
	appendStringInfoChar(out, (char) id->len);
	for (int i = 0; i < id->len; i++)
		appendStringInfoChar(out, (char) ~id->bytes[i]);
}

static void
horizon_corrupt(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("the history horizon's record is corrupt")));
}

static const char *
get_id(const char *p, const char *end, ProllyHash *id)
{
	if (p >= end || (uint8) *p > PROLLY_HASH_MAX_LEN ||
		end - p - 1 < (uint8) *p)
		horizon_corrupt();
	memset(id, 0, sizeof(*id));
	id->len = (uint8) *p;
	memcpy(id->bytes, p + 1, id->len);
	return p + 1 + id->len;
}

static const char *
get_value_id(const char *p, const char *end, ProllyHash *id)
{
	p = get_id(p, end, id);
	for (int i = 0; i < id->len; i++)
		id->bytes[i] = (uint8) ~id->bytes[i];
	return p;
}

static ProllyHash
hash_key(const ProllyHash *h)
{
	ProllyHash	k;

	memset(&k, 0, sizeof(k));
	k.len = h->len;
	memcpy(k.bytes, h->bytes, h->len);
	return k;
}

static bool
same_id(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static HTAB *
id_set(const char *name, Size entrysize)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = entrysize;
	ctl.hcxt = CurrentMemoryContext;
	return hash_create(name, 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static bool
set_has(HTAB *set, const ProllyHash *id)
{
	ProllyHash	k = hash_key(id);

	return hash_search(set, &k, HASH_FIND, NULL) != NULL;
}

static bool
set_add(HTAB *set, const ProllyHash *id)
{
	ProllyHash	k = hash_key(id);
	bool		found;

	(void) hash_search(set, &k, HASH_ENTER, &found);
	return !found;
}

/* A fragment's record, encoded. */
static void
fragment_encode(StringInfo out, const ProllyWindowFragment *f, uint32 generation,
				int nstops, const ProllyHash *stops)
{
	appendStringInfoChar(out, FRAGMENT_VERSION);
	appendStringInfoChar(out, f->deleted ? FRAGMENT_DELETED : 0);
	put_be64(out, (uint64) f->severed_at);
	put_be64(out, (uint64) f->oldest);
	put_be64(out, (uint64) f->newest);
	put_be64(out, (uint64) f->commits);
	put_be64(out, (uint64) f->chunks);
	put_be64(out, (uint64) f->bytes);
	put_be64(out, generation);
	put_value_id(out, &f->root);
	put_value_id(out, &f->digest);
	prolly_put_varint(out, f->location ? strlen(f->location) : 0);
	if (f->location)
		appendStringInfoString(out, f->location);
	prolly_put_varint(out, f->ntops);
	for (int i = 0; i < f->ntops; i++)
		put_value_id(out, &f->tops[i]);
	prolly_put_varint(out, nstops);
	for (int i = 0; i < nstops; i++)
		put_value_id(out, &stops[i]);
}

typedef struct FragmentRecord
{
	ProllyWindowFragment f;
	uint32		generation;
	int			nstops;
	ProllyHash *stops;
} FragmentRecord;

static void
fragment_decode(const char *p, Size len, int64 number, FragmentRecord *r)
{
	const char *end = p + len;
	uint64		n;
	ProllyHash *tops;

	memset(r, 0, sizeof(*r));
	if (len < 2 + 7 * 8 + 8 || p[0] != FRAGMENT_VERSION)
		horizon_corrupt();
	r->f.number = number;
	r->f.deleted = (p[1] & FRAGMENT_DELETED) != 0;
	p += 2;
	r->f.severed_at = (TimestampTz) get_be64(p);
	r->f.oldest = (TimestampTz) get_be64(p + 8);
	r->f.newest = (TimestampTz) get_be64(p + 16);
	r->f.commits = (int64) get_be64(p + 24);
	r->f.chunks = (int64) get_be64(p + 32);
	r->f.bytes = (int64) get_be64(p + 40);
	r->generation = (uint32) get_be64(p + 48);
	p += 56;
	p = get_value_id(p, end, &r->f.root);
	p = get_value_id(p, end, &r->f.digest);
	n = prolly_get_varint(&p, end);
	if (n > (uint64) (end - p))
		horizon_corrupt();
	r->f.location = n > 0 ? pnstrdup(p, n) : NULL;
	p += n;
	n = prolly_get_varint(&p, end);
	if (n > (uint64) (end - p))
		horizon_corrupt();
	tops = palloc_array(ProllyHash, Max(n, 1));
	for (uint64 i = 0; i < n; i++)
		p = get_value_id(p, end, &tops[i]);
	r->f.ntops = (int) n;
	r->f.tops = tops;
	n = prolly_get_varint(&p, end);
	if (n > (uint64) (end - p))
		horizon_corrupt();
	r->stops = palloc_array(ProllyHash, Max(n, 1));
	for (uint64 i = 0; i < n; i++)
		p = get_value_id(p, end, &r->stops[i]);
	r->nstops = (int) n;
}

static void
number_key(StringInfo k, char what, int64 number)
{
	appendStringInfoChar(k, what);
	put_be64(k, (uint64) number);
}

/* ------------------------------------------------------------------------
 * The horizon as read
 * ------------------------------------------------------------------------
 */

/*
 * The parents the horizon names (ProllyHash), read once a transaction:
 * the horizon changes only holding every ref and this lock, and a reader
 * of a horizon a sever moves meanwhile is no worse off than one that read
 * before it.
 */
static HTAB *archived_set = NULL;
static MemoryContext archived_cxt = NULL;
static LocalTransactionId archived_lxid = InvalidLocalTransactionId;
static LocalTransactionId changed_lxid = InvalidLocalTransactionId;

static bool
collect_edge(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	ProllyWindowEdgeVisit visit = ((void **) arg)[0];
	void	   *varg = ((void **) arg)[1];
	const char *p = (const char *) key->data;
	const char *end = p + key->len;
	ProllyHash	parent;
	ProllyHash	boundary;

	if (p >= end || *p != KEY_EDGE)
		return false;
	p = get_id(p + 1, end, &parent);
	(void) get_id(p, end, &boundary);
	visit(&boundary, &parent, varg);
	return true;
}

static void
visit_edges(const ProllyHash *root, ProllyChunkRead read, void *read_arg,
			ProllyWindowEdgeVisit visit, void *arg)
{
	char		e = KEY_EDGE;
	ProllyKey	from = {(const uint8 *) &e, 1};
	void	   *args[2] = {(void *) visit, arg};

	window_init();
	if (root != NULL)
		PROLLY_CALL(dc.refs, side_scan_at, root, read, read_arg,
					SPACE_HORIZON, &from, ForwardScanDirection, collect_edge,
					args);
	else
		PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
					ForwardScanDirection, collect_edge, args);
}

static void
note_archived(const ProllyHash *boundary, const ProllyHash *parent, void *arg)
{
	(void) set_add((HTAB *) arg, parent);
}

static HTAB *
archived_parents(void)
{
	if (archived_set != NULL && archived_lxid == MyProc->vxid.lxid)
		return archived_set;
	if (archived_cxt == NULL)
		archived_cxt = AllocSetContextCreate(TopMemoryContext,
											 "prolly history horizon",
											 ALLOCSET_SMALL_SIZES);
	MemoryContextReset(archived_cxt);
	{
		MemoryContext old = MemoryContextSwitchTo(archived_cxt);

		archived_set = id_set("prolly horizon parents", sizeof(ProllyHash));
		archived_lxid = InvalidLocalTransactionId;
		visit_edges(NULL, NULL, NULL, note_archived, archived_set);
		MemoryContextSwitchTo(old);
	}
	archived_lxid = MyProc->vxid.lxid;
	return archived_set;
}

/*
 * A subtransaction rolled back may have changed the horizon (a sever
 * undone): read it again from here.
 */
static void
window_subxact(SubXactEvent event, SubTransactionId mySubid,
			   SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		archived_lxid = InvalidLocalTransactionId;
}

/* The horizon changed in this transaction: read it again from here. */
static void
horizon_changed(void)
{
	changed_lxid = MyProc->vxid.lxid;
	archived_lxid = InvalidLocalTransactionId;
}

static bool
archive_archived(const ProllyHash *id)
{
	if (!IsTransactionState() || id->len == 0)
		return false;
	window_init();
	if (!PROLLY_HAS(dc.refs, side_scan))
		return false;
	return set_has(archived_parents(), id);
}

/*
 * Severs and repairs take turns: each holds this lock to transaction end,
 * taken before the horizon's (a sever archives holding only it, so
 * collections go on meanwhile).
 */
/* A sever in the background: it takes the severs' turn only if free. */
static bool sever_in_background = false;
static bool sever_busy = false;	/* the last sever found the turn taken */

static bool
sever_slot_lock(void)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, HORIZON_LOCK_CLASSID, 1, 0);
	/*
	 * (in the background, never waited for: a worker waiting on another
	 * gets it cancelled, autovacuum's rule, and the next visit tries again)
	 */
	return LockAcquire(&tag, ExclusiveLock, false, sever_in_background) !=
		LOCKACQUIRE_NOT_AVAIL;
}

static void
horizon_lock(LOCKMODE mode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, HORIZON_LOCK_CLASSID, 0, 0);
	(void) LockAcquire(&tag, mode, false, false);
}

/*
 * The lock order: the horizon's lock first, then the refs' (a sever, a
 * repair), or the chunk store's collection (a collection): so a
 * collection, holding it shared from before it begins until its
 * transaction ends, reads at its seal the horizon as it stands
 * committed, which no sever or repair moves until the collection is done.
 */
static void
archive_hold(void)
{
	window_init();
	if (changed_lxid == MyProc->vxid.lxid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a collection can't run in the transaction that changed the history horizon"),
				 errhint("Commit the transaction first.")));
	horizon_lock(ShareLock);
}

static void
archive_edges(const ProllyHash *refs_root, ProllyChunkRead read,
			  void *read_arg, ProllyWindowEdgeVisit visit, void *arg)
{
	if (changed_lxid == MyProc->vxid.lxid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a collection can't run in the transaction that changed the history horizon"),
				 errhint("Commit the transaction first.")));
	/* (until the transaction ends: no sever or repair meanwhile) */
	horizon_lock(ShareLock);
	visit_edges(refs_root, read, read_arg, visit, arg);
}

static bool
collect_fragment(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	ProllyWindowFragmentVisit visit = ((void **) arg)[0];
	void	   *varg = ((void **) arg)[1];
	FragmentRecord r;

	if (key->len != 9 || key->data[0] != KEY_FRAGMENT)
		return false;
	fragment_decode((const char *) value->data, value->len,
					(int64) get_be64((const char *) key->data + 1), &r);
	visit(&r.f, varg);
	return true;
}

/* Fragments, newest first. */
static void
archive_fragments(ProllyWindowFragmentVisit visit, void *arg)
{
	StringInfoData k;
	ProllyKey	from;
	void	   *args[2] = {(void *) visit, arg};

	window_init();
	initStringInfo(&k);
	number_key(&k, KEY_FRAGMENT, PG_INT64_MAX);
	from.data = (const uint8 *) k.data;
	from.len = k.len;
	PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
				BackwardScanDirection, collect_fragment, args);
	pfree(k.data);
}

/* ------------------------------------------------------------------------
 * Settings
 *
 * The window is the database's: prolly.history_* settings as the database
 * has them (ALTER DATABASE ... SET, which only its owner or a superuser
 * may make), never as a session has them, so no one but those severs more
 * or sends history elsewhere by a SET of their own.  A branch's lineage
 * may keep its own window (dolt_history_window()), in the refs' settings.
 * ------------------------------------------------------------------------
 */

static int	action_setting = PROLLY_WINDOW_ARCHIVE;
static char *warm_setting = NULL;
static int	min_commits_setting = 0;
static char *max_size_setting = NULL;
static char *cold_batch_setting = NULL;
static char *collect_interval_setting = NULL;
static char *archive_timeout_setting = NULL;
static char *archive_setting = NULL;

static const struct config_enum_entry action_options[] = {
	{"archive", PROLLY_WINDOW_ARCHIVE, false},
	{"delete", PROLLY_WINDOW_DELETE, false},
	{"none", PROLLY_WINDOW_NONE, false},
	{NULL, 0, false}
};

/* An interval's microseconds (months as 30 days, as interval_cmp does). */
static bool
parse_span(const char *value, int64 *usec)
{
	Datum		d;
	Interval   *iv;

	if (value == NULL || *value == '\0')
	{
		*usec = 0;
		return true;
	}
	if (!DirectInputFunctionCallSafe(interval_in, value, InvalidOid, -1, NULL,
									 &d))
		return false;
	iv = DatumGetIntervalP(d);
	if (INTERVAL_NOT_FINITE(iv))
		return false;
	*usec = iv->time + (int64) iv->day * USECS_PER_DAY +
		(int64) iv->month * DAYS_PER_MONTH * USECS_PER_DAY;
	return *usec >= 0;
}

static bool
parse_size(const char *value, int64 *bytes)
{
	Datum		d;

	if (value == NULL || *value == '\0')
	{
		*bytes = 0;
		return true;
	}
	PG_TRY();
	{
		d = DirectFunctionCall1(pg_size_bytes, CStringGetTextDatum(value));
		*bytes = DatumGetInt64(d);
	}
	PG_CATCH();
	{
		FlushErrorState();
		return false;
	}
	PG_END_TRY();
	return *bytes >= 0;
}

static bool
check_span(char **newval, void **extra, GucSource source)
{
	int64		usec;

	if (!parse_span(*newval, &usec))
	{
		GUC_check_errdetail("Give a non-negative interval, such as \"7 days\", or nothing.");
		return false;
	}
	return true;
}

/* The archive is a remote's URL (not a remote's name, which others may move). */
static bool
check_archive(char **newval, void **extra, GucSource source)
{
	if (*newval != NULL && **newval != '\0' && strstr(*newval, "://") == NULL)
	{
		GUC_check_errdetail("Give a remote's URL, such as file:///path, not a remote's name.");
		return false;
	}
	return true;
}

static bool
check_size(char **newval, void **extra, GucSource source)
{
	int64		bytes;

	if (!parse_size(*newval, &bytes))
	{
		GUC_check_errdetail("Give a size, such as \"500 GB\", or nothing.");
		return false;
	}
	return true;
}

void
prolly_window_init(void)
{
	static bool defined = false;

	if (defined)
		return;
	defined = true;
	DefineCustomEnumVariable("prolly.history_window_action",
							 "What is done with history past its window: archive, delete or none.",
							 "Read as the database has it (ALTER DATABASE ... SET), never a session's.",
							 &action_setting, PROLLY_WINDOW_ARCHIVE,
							 action_options, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomStringVariable("prolly.history_warm",
							   "How long each branch's history is kept here (empty: all of it).",
							   "Read as the database has it; a branch may keep its own (dolt_history_window()).",
							   &warm_setting, "", PGC_USERSET, 0,
							   check_span, NULL, NULL);
	DefineCustomIntVariable("prolly.history_min_commits",
							"How many of each branch's newest commits are kept here, however old.",
							"Read as the database has it; a branch may keep its own.",
							&min_commits_setting, 0, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("prolly.history_max_size",
							   "The most the chunk store may hold (empty: no limit); past it, background severing cuts a batch more each visit.",
							   "Read as the database has it; each branch still keeps its newest prolly.history_min_commits.",
							   &max_size_setting, "", PGC_USERSET, 0,
							   check_size, NULL, NULL);
	DefineCustomStringVariable("prolly.history_collect_interval",
							   "The least time between the full collections that free severed history in the background (empty: an hour).",
							   "Read as the database has it; fragments severed meanwhile share the next one.",
							   &collect_interval_setting, "", PGC_USERSET, 0,
							   check_span, NULL, NULL);
	DefineCustomStringVariable("prolly.history_archive_timeout",
							   "How long a sever waits for its archive to take the history severed and give it back (empty: ten minutes).",
							   "Read as the database has it; past it, nothing is severed.",
							   &archive_timeout_setting, "", PGC_USERSET, 0,
							   check_span, NULL, NULL);
	DefineCustomStringVariable("prolly.history_cold_batch",
							   "Severed history is cut at multiples of this (empty: anywhere), so fragments are whole hours or days.",
							   "Read as the database has it; a branch may keep its own.",
							   &cold_batch_setting, "", PGC_USERSET, 0,
							   check_span, NULL, NULL);
	DefineCustomStringVariable("prolly.history_archive",
							   "Where history past the window is archived: a remote's URL.",
							   "Read as the database has it, opened as the database's owner; with none, archive mode severs nothing.",
							   &archive_setting, "", PGC_USERSET, 0,
							   check_archive, NULL, NULL);
}

/*
 * The database's own settings (no role's), read once a command: a visit
 * evaluates every lineage's policy, a dozen settings each, by one read.
 */
static List *db_settings = NIL;
static MemoryContext db_settings_cxt = NULL;
static LocalTransactionId db_settings_lxid = InvalidLocalTransactionId;
static CommandId db_settings_cid = InvalidCommandId;

static List *
database_settings(void)
{
	Relation	rel;
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	MemoryContext old;
	CommandId	cid = GetCurrentCommandId(false);

	if (db_settings_cxt != NULL && db_settings_lxid == MyProc->vxid.lxid &&
		db_settings_cid == cid)
		return db_settings;
	if (db_settings_cxt == NULL)
		db_settings_cxt = AllocSetContextCreate(TopMemoryContext,
												"prolly history settings",
												ALLOCSET_SMALL_SIZES);
	MemoryContextReset(db_settings_cxt);
	db_settings = NIL;
	old = MemoryContextSwitchTo(db_settings_cxt);
	rel = table_open(DbRoleSettingRelationId, AccessShareLock);
	ScanKeyInit(&keys[0], Anum_pg_db_role_setting_setdatabase,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(MyDatabaseId));
	ScanKeyInit(&keys[1], Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(InvalidOid));
	scan = systable_beginscan(rel, DbRoleSettingDatidRolidIndexId, true,
							  NULL, 2, keys);
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;
		Datum		d = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
									 RelationGetDescr(rel), &isnull);

		if (!isnull)
		{
			ArrayType  *a = DatumGetArrayTypeP(d);
			Datum	   *elems;
			int			n;

			deconstruct_array_builtin(a, TEXTOID, &elems, NULL, &n);
			for (int i = 0; i < n; i++)
				db_settings = lappend(db_settings, TextDatumGetCString(elems[i]));
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	MemoryContextSwitchTo(old);
	db_settings_lxid = MyProc->vxid.lxid;
	db_settings_cid = cid;
	return db_settings;
}

/* name's value as the database has it (its own, no role's); NULL: none. */
static char *
database_setting(const char *name)
{
	size_t		len = strlen(name);
	char	   *found = NULL;

	foreach_ptr(char, s, database_settings())
		if (strncmp(s, name, len) == 0 && s[len] == '=')
			found = pstrdup(s + len + 1);
	return found;
}

static int64
database_span(const char *name)
{
	char	   *v = database_setting(name);
	int64		usec = 0;

	if (v && !parse_span(v, &usec))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the database's %s isn't an interval: \"%s\"", name, v)));
	return usec;
}

static const char *const branch_settings[] = {"warm", "min_commits",
"cold_batch"};

static void
branch_setting_key(StringInfo key, const char *name, const char *setting)
{
	appendStringInfo(key, "history_window.%s:%s", setting, name);
}

/* A branch lineage's own setting; NULL: none. */
static char *
branch_setting(const char *name, const char *setting)
{
	StringInfoData key;
	StringInfoData v;
	char	   *found = NULL;

	initStringInfo(&key);
	initStringInfo(&v);
	branch_setting_key(&key, name, setting);
	if (PROLLY_CALL(dc.refs, config_get, key.data, &v))
		found = pstrdup(v.data);
	pfree(key.data);
	pfree(v.data);
	return found;
}

/*
 * The warm window holds the hot one (prolly.history_hot, the page roots'
 * window, as the database has it, where that is set): a warm window
 * shorter is taken as hot, with a notice.
 */
static void
hold_hot(const char *name, ProllyWindowPolicy *p)
{
	int64		hot = database_span("prolly.history_hot");

	/*
	 * (any window, by time, by count or by size: what is younger than hot
	 * is kept)
	 */
	if (hot > p->keep_time &&
		(p->keep_time > 0 || p->keep_commits > 0 || p->max_bytes > 0))
	{
		ereport(NOTICE,
				(errmsg("the history window of %s%s%s is taken as prolly.history_hot, which it can't be shorter than",
						name ? "branch \"" : "database \"",
						name ? name : get_database_name(MyDatabaseId), "\"")));
		p->keep_time = hot;
	}
}

static void
archive_policy(const char *name, ProllyWindowPolicy *p)
{
	char	   *v;

	window_init();
	memset(p, 0, sizeof(*p));
	v = database_setting("prolly.history_window_action");
	p->action = PROLLY_WINDOW_ARCHIVE;
	if (v)
	{
		for (int i = 0; action_options[i].name; i++)
			if (pg_strcasecmp(v, action_options[i].name) == 0)
				p->action = action_options[i].val;
	}
	p->keep_time = database_span("prolly.history_warm");
	p->batch = database_span("prolly.history_cold_batch");
	if ((v = database_setting("prolly.history_min_commits")) != NULL)
		p->keep_commits = strtol(v, NULL, 10);
	if ((v = database_setting("prolly.history_max_size")) != NULL &&
		!parse_size(v, &p->max_bytes))
		p->max_bytes = 0;
	v = database_setting("prolly.history_archive");
	p->archive = (v && *v) ? v : NULL;
	if (name == NULL)
	{
		hold_hot(NULL, p);
		return;
	}
	if ((v = branch_setting(name, "warm")) != NULL)
		(void) parse_span(v, &p->keep_time);
	if ((v = branch_setting(name, "cold_batch")) != NULL)
		(void) parse_span(v, &p->batch);
	if ((v = branch_setting(name, "min_commits")) != NULL)
		p->keep_commits = strtol(v, NULL, 10);
	hold_hot(name, p);
}

/* A branch lineage's own setting (value NULL: the database's again). */
static void
archive_set_policy(const char *name, const char *setting, const char *value)
{
	StringInfoData key;
	bool		known = false;

	window_init();
	check_may_sever("set the window of");
	for (int i = 0; i < lengthof(branch_settings); i++)
		known |= strcmp(setting, branch_settings[i]) == 0;
	if (!known)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a branch's history window has no setting \"%s\"", setting),
				 errhint("A branch keeps its own warm, min_commits or cold_batch; the rest are the database's.")));
	if (value != NULL)
	{
		int64		n;

		if (strcmp(setting, "min_commits") == 0)
		{
			char	   *end;

			n = strtol(value, &end, 10);
			if (*end != '\0' || n < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("min_commits must be a count, not \"%s\"", value)));
		}
		else if (!parse_span(value, &n))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s must be an interval, not \"%s\"", setting, value)));
	}
	initStringInfo(&key);
	branch_setting_key(&key, name, setting);
	PROLLY_CALL(dc.refs, config_set, key.data, value);
	pfree(key.data);
}

/* ------------------------------------------------------------------------
 * Plans
 * ------------------------------------------------------------------------
 */

/* What a plan knows of a commit: as the commit records it. */
typedef struct PlanEntry
{
	TimestampTz committed_at;
	int64		height;
	int			nparents;
	ProllyHash	parents[PROLLY_MAX_PARENTS];
} PlanEntry;

/* A commit of a plan's walk. */
typedef struct PlanCommit
{
	ProllyHash	id;				/* key */
	PlanEntry	e;
	TimestampTz young_cut;		/* young() as of this cut (DT_NOBEGIN: none) */
	TimestampTz arrived;		/* dated past now: when first seen here */
	bool		young;
	bool		kept;
	bool		pinned;			/* kept as a ref's commit, not a window's */
	bool		by_base;		/* kept only on the way to a merge base */
} PlanCommit;

/*
 * A commit a ref names, whose window is walked: a branch's head (name
 * set), or another ref's commit (a tag's, a remote-tracking branch's, a
 * session's detached head, a stash's, a merge's or rebase's in progress, a
 * pin's, a kept one's), which keeps the database's window.
 */
typedef struct Head
{
	char	   *name;			/* the branch's; NULL: another ref's */
	ProllyHash	head;
	int64		floor;			/* keeps every commit at least this high */
	TimestampTz cut;			/* and every commit dated at or after it */
	int64		min_commits;	/* and its newest this many */
	bool		standalone;		/* a tag's: kept itself, on a lineage too */
	bool		on_lineage;		/* another ref's, on a branch's history */
} Head;

typedef struct Plan
{
	HTAB	   *commits;		/* PlanCommit, by id */
	List	   *heads;			/* Head *, every commit a ref names */
	HTAB	   *head_ids;		/* their ids, each once */
	List	   *kept;			/* PlanCommit *, the window */
	List	   *edges;			/* ProllyHash *, pairs: boundary, parent */
	List	   *severed;		/* PlanCommit *, the fragment */
	List	   *stops;			/* ProllyHash *, outside it, its commits name */
	HTAB	   *archived;		/* the horizon's parents now */
	HTAB	   *heights;		/* of commits recording none */
	List	   *arrivals;		/* PlanCommit *: dated past now, first seen */
	HTAB	   *moved;			/* HeadArrival: the head logs' first moves */
} Plan;

static bool
lookup_commit(const ProllyHash *id, ProllyCommitInfo *info, void *arg)
{
	return PROLLY_CALL(dc.refs, commit_get, id, info);
}

static PlanCommit *
plan_commit(Plan *plan, const ProllyHash *id)
{
	ProllyHash	k = hash_key(id);
	PlanCommit *pc;
	bool		found;

	pc = hash_search(plan->commits, &k, HASH_ENTER, &found);
	if (!found)
	{
		ProllyCommitInfo info;

		pc->kept = false;
		pc->pinned = false;
		pc->by_base = false;
		pc->young_cut = DT_NOBEGIN;
		pc->young = false;
		pc->arrived = DT_NOBEGIN;
		if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("commit %s is missing from the chunk store",
							prolly_hash_hex(id))));
		pc->e.committed_at = info.committed_at;
		pc->e.nparents = info.nparents;
		for (int i = 0; i < info.nparents; i++)
			pc->e.parents[i] = info.parents[i];
		if (info.has_height)
			pc->e.height = info.height;
		else
		{
			if (plan->heights == NULL)
				plan->heights = prolly_history_heights();
			pc->e.height = prolly_history_height(id, plan->heights,
												 lookup_commit, NULL);
		}
	}
	return pc;
}

static void
add_head(Plan *plan, const char *name, const ProllyHash *head)
{
	Head	   *h;

	/* (a commit several refs name is walked once, a branch's first) */
	if (head->len == 0 || !set_add(plan->head_ids, head))
		return;
	h = palloc0(sizeof(Head));
	h->name = name ? pstrdup(name) : NULL;
	h->head = *head;
	h->floor = PG_INT64_MAX;
	plan->heads = lappend(plan->heads, h);
}

static void
note_branch(const char *name, const ProllyHash *head, void *arg)
{
	Plan	   *plan = arg;

	/* (a session's detached head is another ref's commit) */
	add_head(plan,
			 strncmp(name, PROLLY_DETACHED_PREFIX,
					 strlen(PROLLY_DETACHED_PREFIX)) == 0 ? NULL : name,
			 head);
}

/* Every chunk the refs' entries name, as a collection reads them. */
typedef struct RefsNamed
{
	List	  **named;
	StringInfoData buf;
} RefsNamed;

static void
note_named(const ProllyHash *ref, void *arg)
{
	RefsNamed  *rn = arg;

	*rn->named = lappend(*rn->named, memcpy(palloc(sizeof(ProllyHash)), ref,
											sizeof(ProllyHash)));
}

static bool
read_refs_node(const ProllyNodeRef *node, uint16 level, void *arg)
{
	RefsNamed  *rn = arg;

	resetStringInfo(&rn->buf);
	if (!PROLLY_CALL(tree_env.chunk_store, get, tree_env.chunks, &node->hash,
					 &rn->buf))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the refs' node %s is missing from the chunk store",
						prolly_hash_hex(&node->hash))));
	/* (a leaf's: an inner node's are its children, walked anyway) */
	if (level == 0)
		prolly_chunk_references(rn->buf.data, rn->buf.len, note_named, rn);
	return true;
}

/*
 * The commits the refs name that aren't branch heads, each with a window
 * of its own: every entry's commits, as a collection reads them (a tag's
 * object names its commit).  So whatever names a commit keeps its window,
 * whatever kind of ref it is, a kind made later too.
 */
static void
note_lineage(const char *name, const ProllyHash *head, void *arg)
{
	List	  **names = arg;

	if (strncmp(name, PROLLY_DETACHED_PREFIX,
				strlen(PROLLY_DETACHED_PREFIX)) != 0)
		*names = lappend(*names, pstrdup(name));
}

static void
note_kept(const ProllyKeptCommit *k, void *arg)
{
	(void) set_add((HTAB *) arg, &k->commit);
}

static void
note_refs(Plan *plan)
{
	ProllyHash	root;
	ProllyNodeRef ref;
	List	   *named = NIL;
	RefsNamed	rn = {&named};
	ListCell   *lc;

	HTAB	   *kept = id_set("prolly kept commits", sizeof(ProllyHash));

	PROLLY_CALL(tree_env.chunk_store, root, tree_env.chunks, &root);
	if (root.len == 0)
		return;
	ref.hash = root;
	initStringInfo(&rn.buf);
	(void) PROLLY_CALL(tree, walk, &tree_env, &ref, read_refs_node, &rn);

	/*
	 * (the commits a branch's pages are kept as of are its lineage's, its
	 * window's to keep or cut: no window of their own)
	 */
	if (PROLLY_HAS(dc.refs, kept_commits))
	{
		List	   *names = NIL;

		PROLLY_CALL(dc.refs, branches, note_lineage, &names);
		foreach_ptr(char, name, names)
			PROLLY_CALL(dc.refs, kept_commits, name, note_kept, kept);
	}
	foreach(lc, named)
	{
		ProllyHash *h = lfirst(lc);
		ProllyCommitInfo info;
		ProllyTagInfo tag;

		if (set_has(kept, h) ||
			!PROLLY_CALL(tree_env.chunk_store, has, tree_env.chunks, h))
			continue;
		if (PROLLY_CALL(dc.refs, commit_get, h, &info))
			add_head(plan, NULL, h);
		else if (PROLLY_HAS(dc.refs, tag_get) &&
				 PROLLY_CALL(dc.refs, tag_get, h, &tag))
		{
			add_head(plan, NULL, &tag.commit);
			foreach_ptr(Head, th, plan->heads)
				if (same_id(&th->head, &tag.commit))
					th->standalone = true;
		}
	}
	pfree(rn.buf.data);
	hash_destroy(kept);
}

/*
 * Each branch head, and each ref's commit it may be merged with, keeps the
 * commits down to every merge base of the two (every maximal common
 * ancestor, where histories cross): a merge into a branch is from another
 * branch, a tag, a remote-tracking branch or any ref's commit, so every
 * pair of a branch and a ref is one.  (Two refs that aren't branches merge
 * only into a branch, which pairs with each.)
 */
static void
note_merge_bases(Plan *plan)
{
	int			n = list_length(plan->heads);

	for (int i = 0; i < n; i++)
	{
		Head	   *a = list_nth(plan->heads, i);

		if (a->name == NULL)
			continue;
		for (int j = 0; j < n; j++)
		{
			Head	   *b = list_nth(plan->heads, j);
			ProllyHash *bases;
			int			nbases;

			if (j == i || (b->name != NULL && j < i))
				continue;		/* (each pair of branches once) */
			nbases = prolly_history_merge_bases(&a->head, &b->head,
												lookup_commit, NULL, &bases);

			/*
			 * (a ref's commit on the branch's history, but a tag's, is the
			 * branch's lineage: no window, nor path, of its own; a tag's
			 * keeps the path to it)
			 */
			if (b->name == NULL && nbases == 1 && same_id(&bases[0], &b->head))
			{
				b->on_lineage = true;
				if (!b->standalone)
					continue;
			}
			for (int k = 0; k < nbases; k++)
			{
				PlanCommit *pc = plan_commit(plan, &bases[k]);

				a->floor = Min(a->floor, pc->e.height);
				b->floor = Min(b->floor, pc->e.height);
			}
		}
	}

	/*
	 * A ref's commit on a branch's history, but a tag's, has no window of
	 * its own: the commits a branch's pages are kept as of, a reader's pin,
	 * a remote-tracking branch are the branch's lineage, its window's to
	 * keep or cut (what a ref names a collection keeps whatever the
	 * horizon says, until the ref lets go of it).  A tag keeps itself.
	 */
	foreach_ptr(Head, h, plan->heads)
		if (h->on_lineage && !h->standalone)
			plan->heads = foreach_delete_current(plan->heads, h);
}

/*
 * Whether a commit is younger than cut, by a clock that only goes forward
 * along history: it, or a commit beneath it (this side of the horizon), is
 * dated at cut or later, a date past now taken as now.  Commit dates
 * needn't increase along history (one made with --date, a machine's clock
 * skewed, an import), so by this clock no commit is older than one beneath
 * it, and a window, the commits younger than its cut, is a prefix of every
 * path: nothing is severed beneath a commit kept for its age.  (A commit
 * dated in the future keeps itself and what is above it until that date
 * is past.)  Found once for each commit and cut, walking down only past
 * commits older than the cut: O(what a sever walks anyway).
 */
/*
 * A sever's clock: fixed when it begins, so the plans it makes again (the
 * refs moved meanwhile) cut where the first did, and only take in the
 * commits made since above the cut.  DT_NOBEGIN: no sever's, now.
 */
static TimestampTz sever_clock = DT_NOBEGIN;

static TimestampTz
plan_now(void)
{
	return sever_clock != DT_NOBEGIN ? sever_clock : GetCurrentTimestamp();
}

#define KEY_ARRIVED		'a'

/* A commit a head log names: the first move to it, by its date. */
typedef struct HeadArrival
{
	ProllyHash	id;				/* key */
	TimestampTz at;
} HeadArrival;

static void
note_any_branch(const char *name, const ProllyHash *head, void *arg)
{
	List	  **names = arg;

	*names = lappend(*names, pstrdup(name));
}

static bool
note_move(const ProllyHeadMove *move, void *arg)
{
	HTAB	   *moved = arg;
	ProllyHash	k = hash_key(&move->commit);
	HeadArrival *a;
	bool		found;

	a = hash_search(moved, &k, HASH_ENTER, &found);
	if (!found || move->moved_at < a->at)
		a->at = move->moved_at;
	return true;
}

/*
 * Every branch's head log read once, for the first move to each commit.
 * Only a plan meeting a commit dated past now that no sever has recorded
 * reads them (each such commit's arrival is then recorded), so the read,
 * as long as the logs are, is paid once for it, not by every plan.
 */
static HTAB *
head_arrivals(void)
{
	HTAB	   *moved = id_set("prolly head arrivals", sizeof(HeadArrival));
	List	   *names = NIL;

	if (!PROLLY_HAS(dc.refs, head_moves))
		return moved;
	PROLLY_CALL(dc.refs, branches, note_any_branch, &names);
	foreach_ptr(char, name, names)
		PROLLY_CALL(dc.refs, head_moves, name, NULL, DT_NOBEGIN, DT_NOEND,
					ForwardScanDirection, note_move, moved);
	list_free_deep(names);
	return moved;
}

/*
 * A commit dated past now ages from when it arrived here: the first move
 * of a branch's head to it, as the head log dates it (when it was made
 * here, or reset, merged or fast-forwarded to); where no log names it (it
 * came in beneath another), when a sever's plan first saw it; now if none
 * has.  Kept beside the horizon ('a') by the sever that first finds it.
 * So one --date in the future, or a pull of a clock skewed ahead, keeps
 * the history above it no longer than any other, and a warning says so.
 */
static TimestampTz
age_date(Plan *plan, PlanCommit *pc)
{
	StringInfoData k;
	StringInfoData v;
	ProllyKey	key;

	/* (ahead of the clock as it is, not as a sever fixed it) */
	TimestampTz now = GetCurrentTimestamp();

	if (pc->e.committed_at <= now)
		return pc->e.committed_at;
	if (pc->arrived != DT_NOBEGIN)
		return pc->arrived;
	initStringInfo(&k);
	initStringInfo(&v);
	appendStringInfoChar(&k, KEY_ARRIVED);
	put_id(&k, &pc->id);
	key.data = (const uint8 *) k.data;
	key.len = k.len;
	if (PROLLY_CALL(dc.refs, side_get, SPACE_HORIZON, &key, &v) && v.len == 8)
		pc->arrived = (TimestampTz) get_be64(v.data);
	else
	{
		ProllyHash	hk = hash_key(&pc->id);
		HeadArrival *a;

		if (plan->moved == NULL)
			plan->moved = head_arrivals();
		a = hash_search(plan->moved, &hk, HASH_FIND, NULL);
		pc->arrived = a != NULL ? Min(a->at, now) : now;
		plan->arrivals = lappend(plan->arrivals, pc);
		ereport(WARNING,
				(errmsg("commit %s is dated %s, after it arrived here",
						prolly_hash_hex(&pc->id),
						timestamptz_to_str(pc->e.committed_at)),
				 errdetail("It ages from %s instead.",
						   timestamptz_to_str(pc->arrived))));
	}
	pfree(k.data);
	pfree(v.data);
	return pc->arrived;
}

/* Keep the arrivals a sever's plan saw first.  The caller holds the refs. */
static void
record_arrivals(Plan *plan)
{
	foreach_ptr(PlanCommit, pc, plan->arrivals)
	{
		StringInfoData k;
		StringInfoData v;
		ProllyKey	key;
		ProllyValue value;

		initStringInfo(&k);
		initStringInfo(&v);
		appendStringInfoChar(&k, KEY_ARRIVED);
		put_id(&k, &pc->id);
		put_be64(&v, (uint64) pc->arrived);
		key.data = (const uint8 *) k.data;
		key.len = k.len;
		value.data = (const uint8 *) v.data;
		value.len = v.len;
		PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
		pfree(k.data);
		pfree(v.data);
	}
}

static bool
young(Plan *plan, PlanCommit *pc, TimestampTz cut)
{
	List	   *stack;

	if (pc->young_cut == cut && cut != DT_NOBEGIN)
		return pc->young;
	stack = list_make1(pc);
	while (stack != NIL)
	{
		PlanCommit *top = llast(stack);
		bool		pending = false;
		bool		result = age_date(plan, top) >= cut;

		if (top->young_cut == cut && cut != DT_NOBEGIN)
		{
			stack = list_delete_last(stack);
			continue;
		}
		for (int i = 0; !result && i < top->e.nparents; i++)
		{
			PlanCommit *p;

			if (set_has(plan->archived, &top->e.parents[i]))
				continue;
			p = plan_commit(plan, &top->e.parents[i]);
			if (p->young_cut != cut)
			{
				stack = lappend(stack, p);
				pending = true;
			}
			else
				result = p->young;
		}
		if (pending && !result)
			continue;
		top->young = result;
		top->young_cut = cut;
		stack = list_delete_last(stack);
		CHECK_FOR_INTERRUPTS();
	}
	return pc->young;
}

static void
keep(Plan *plan, PlanCommit *pc)
{
	if (pc->kept)
		return;
	pc->kept = true;
	plan->kept = lappend(plan->kept, pc);
}

/* A branch head's window: walked from it down to what it doesn't keep. */
static void
walk_window(Plan *plan, Head *h)
{
	List	   *todo = list_make1(&h->head);
	HTAB	   *seen = id_set("prolly window walk", sizeof(ProllyHash));
	int64		head_height = plan_commit(plan, &h->head)->e.height;

	(void) set_add(seen, &h->head);
	while (todo != NIL)
	{
		ProllyHash *id = llast(todo);
		PlanCommit *pc;

		todo = list_delete_last(todo);
		CHECK_FOR_INTERRUPTS();
		pc = plan_commit(plan, id);
		if (same_id(id, &h->head) || young(plan, pc, h->cut) ||
			(h->min_commits > 0 && head_height - pc->e.height < h->min_commits))
		{
			if (pc->kept && pc->by_base)
				pc->by_base = false;
		}
		else if (pc->e.height >= h->floor)
		{
			if (!pc->kept)
				pc->by_base = true;
		}
		else
			continue;
		keep(plan, pc);
		for (int i = 0; i < pc->e.nparents; i++)
		{
			ProllyHash *p = &pc->e.parents[i];

			if (set_has(plan->archived, p) || !set_add(seen, p))
				continue;
			todo = lappend(todo, p);
		}
	}
	hash_destroy(seen);
}

static void
add_pair(List **list, const ProllyHash *a, const ProllyHash *b)
{
	ProllyHash *pair = palloc_array(ProllyHash, 2);

	pair[0] = *a;
	pair[1] = *b;
	*list = lappend(*list, pair);
}

/*
 * Each head's window: its commits dated before before (if given) cut;
 * else its lineage's policy's, as of now, the cut rounded down to a whole
 * batch.  False if no head's window cuts anything.
 */
static bool
head_windows(Plan *plan, const TimestampTz *before, TimestampTz size_cut)
{
	TimestampTz now = plan_now();
	int64		hot = database_span("prolly.history_hot");

	/* (what is younger than hot is kept, whatever cut is given) */
	TimestampTz hot_cut = hot > 0 ? now - hot : DT_NOEND;
	bool		any = false;

	foreach_ptr(Head, h, plan->heads)
	{
		ProllyWindowPolicy p;

		if (before != NULL)
		{
			h->cut = Min(*before, hot_cut);
			h->min_commits = 0;
			any = true;
			continue;
		}
		archive_policy(h->name, &p);
		h->min_commits = p.keep_commits;
		/*
		 * (another ref's commit, a tag's, a stash's, a branch's page roots'
		 * or log's, keeps itself and the database's min_commits: what is
		 * on a branch's history is that branch's window's to keep, so a
		 * branch's window narrower than the database's cuts it)
		 */
		if (h->name == NULL)
		{
			h->cut = DT_NOEND;
			h->min_commits = Max(p.keep_commits, 1);
			continue;
		}
		if (p.keep_time > 0)
		{
			h->cut = now - p.keep_time;
			if (p.batch > 0)
				h->cut -= ((h->cut % p.batch) + p.batch) % p.batch;
			any = true;
		}
		else if (p.keep_commits > 0)
		{
			h->cut = DT_NOEND;	/* (only the count keeps) */
			any = true;
		}
		else
			h->cut = DT_NOBEGIN;	/* (it keeps everything) */
		/*
		 * (a size limit cuts later, the count still keeping its own, and
		 * hot what is younger)
		 */
		size_cut = Min(size_cut, hot_cut);
		if (size_cut > h->cut)
		{
			h->cut = size_cut;
			any = true;
		}
	}
	return any;
}

/*
 * The plan (before: every head's window cut there; NULL: as the policy
 * has it): the window, the boundary edges from it, and the fragment
 * beyond them.  The caller holds every ref.
 */
static void
make_plan(Plan *plan, const TimestampTz *before, TimestampTz size_cut)
{
	ListCell   *lc;
	HTAB	   *fragment;
	List	   *todo = NIL;

	memset(plan, 0, sizeof(*plan));
	plan->commits = id_set("prolly window plan", sizeof(PlanCommit));
	plan->head_ids = id_set("prolly window heads", sizeof(ProllyHash));
	plan->archived = archived_parents();
	PROLLY_CALL(dc.refs, branches, note_branch, plan);
	note_refs(plan);
	if (!head_windows(plan, before, size_cut))
		return;					/* (every head keeps its whole history) */
	note_merge_bases(plan);
	foreach(lc, plan->heads)
	{
		Head	   *h = lfirst(lc);

		walk_window(plan, h);
		/* (a ref's commit older than its window: kept as the ref's) */
		if (h->name == NULL)
		{
			PlanCommit *pc = plan_commit(plan, &h->head);

			if (pc->e.committed_at < h->cut)
				pc->pinned = true;
		}
	}

	/* the boundary: each parent of a kept commit that isn't kept */
	foreach(lc, plan->kept)
	{
		PlanCommit *pc = lfirst(lc);

		for (int i = 0; i < pc->e.nparents; i++)
		{
			ProllyHash *p = &pc->e.parents[i];
			PlanCommit *parent;

			if (set_has(plan->archived, p))
				continue;
			parent = plan_commit(plan, p);
			if (!parent->kept)
			{
				add_pair(&plan->edges, &pc->id, p);
				todo = lappend(todo, p);
			}
		}
	}

	/* the fragment: what the new tops reach that isn't kept or past */
	fragment = id_set("prolly window fragment", sizeof(ProllyHash));
	{
		HTAB	   *stops = id_set("prolly window stops", sizeof(ProllyHash));
		List	   *tops = todo;

		/* (a top two boundary commits name is walked from once) */
		todo = NIL;
		foreach(lc, tops)
			if (set_add(fragment, lfirst(lc)))
				todo = lappend(todo, lfirst(lc));
		while (todo != NIL)
		{
			ProllyHash *id = llast(todo);
			PlanCommit *pc;

			todo = list_delete_last(todo);
			CHECK_FOR_INTERRUPTS();
			pc = plan_commit(plan, id);
			plan->severed = lappend(plan->severed, pc);
			for (int i = 0; i < pc->e.nparents; i++)
			{
				ProllyHash *p = &pc->e.parents[i];

				if (set_has(fragment, p))
					continue;
				if (set_has(plan->archived, p) || plan_commit(plan, p)->kept)
				{
					if (set_add(stops, p))
						plan->stops = lappend(plan->stops, p);
					continue;
				}
				(void) set_add(fragment, p);
				todo = lappend(todo, p);
			}
		}
		hash_destroy(stops);
	}
	hash_destroy(fragment);
}

static void
archive_plan(const TimestampTz *before, ProllyWindowItemVisit visit, void *arg)
{
	Plan		plan;
	ListCell   *lc;

	sever_clock = DT_NOBEGIN;
	window_init();
	/* (read as the refs stand: no ref is held, nothing written) */
	make_plan(&plan, before, DT_NOBEGIN);
	foreach(lc, plan.severed)
		visit(PROLLY_WINDOW_SEVERED, &((PlanCommit *) lfirst(lc))->id, NULL,
			  NULL, arg);
	foreach(lc, plan.edges)
	{
		ProllyHash *pair = lfirst(lc);

		visit(PROLLY_WINDOW_EDGE, &pair[0], &pair[1], NULL, arg);
	}
	foreach(lc, plan.kept)
	{
		PlanCommit *pc = lfirst(lc);

		if (pc->pinned || pc->by_base)
			visit(PROLLY_WINDOW_PINNED, &pc->id, NULL,
				  pc->pinned ? "ref" : "merge base", arg);
	}
}

/* ------------------------------------------------------------------------
 * Fragments
 * ------------------------------------------------------------------------
 */

typedef struct Closure
{
	HTAB	   *seen;
	List	   *chunks;			/* ProllyHash * */
	int64		bytes;
	StringInfoData buf;
} Closure;

static bool
closure_visit(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Closure    *c = arg;

	if (!set_add(c->seen, &node->hash))
		return false;
	c->chunks = lappend(c->chunks, memcpy(palloc(sizeof(ProllyHash)),
										  &node->hash, sizeof(ProllyHash)));
	resetStringInfo(&c->buf);
	if (PROLLY_CALL(tree_env.chunk_store, get, tree_env.chunks, &node->hash,
					&c->buf))
		c->bytes += c->buf.len;
	return true;
}

static int
hash_cmp(const ListCell *a, const ListCell *b)
{
	const ProllyHash *x = lfirst(a);
	const ProllyHash *y = lfirst(b);

	if (x->len != y->len)
		return x->len < y->len ? -1 : 1;
	return memcmp(x->bytes, y->bytes, x->len);
}

/*
 * What a fragment's commits reach, without crossing to other commits:
 * each one's own tree, its root's (the canonicalizer's walk) and its
 * dependencies'.  The digest is a hash over their hashes, sorted.
 */
static void
fragment_closure(List *commits, ProllyHash *digest, int64 *chunks,
				 int64 *bytes, List **chunk_list)
{
	Closure		c = {0};
	ListCell   *lc;
	ProllyHashCtx *ctx;

	c.seen = id_set("prolly fragment chunks", sizeof(ProllyHash));
	initStringInfo(&c.buf);
	foreach(lc, commits)
	{
		ProllyHash *id = lfirst(lc);
		ProllyCommitInfo info;
		ProllyNodeRef ref;

		CHECK_FOR_INTERRUPTS();
		if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
		{
			prolly_window_check_missing(id);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("commit %s is missing from the chunk store",
							prolly_hash_hex(id))));
		}
		ref.hash = *id;
		if (!PROLLY_CALL(tree, walk, &tree_env, &ref, closure_visit, &c))
			continue;
		PROLLY_CALL(dc.canonicalizer, walk, &info.root, closure_visit, &c);
		if (info.has_dependencies)
			(void) PROLLY_CALL(tree, walk, &tree_env, &info.dependencies,
							   closure_visit, &c);
	}
	list_sort(c.chunks, hash_cmp);
	ctx = PROLLY_CALL(tree_env.hash, begin, CurrentMemoryContext);
	foreach(lc, c.chunks)
	{
		ProllyHash *h = lfirst(lc);

		PROLLY_CALL(tree_env.hash, update, ctx, &h->len, 1);
		PROLLY_CALL(tree_env.hash, update, ctx, h->bytes, h->len);
	}
	PROLLY_CALL(tree_env.hash, finish, ctx, digest);
	*chunks = list_length(c.chunks);
	*bytes = c.bytes;
	if (chunk_list)
		*chunk_list = c.chunks;
	pfree(c.buf.data);
	hash_destroy(c.seen);
}

static int
entry_cmp(const ListCell *a, const ListCell *b)
{
	const PlanCommit *x = lfirst(a);
	const PlanCommit *y = lfirst(b);

	if (x->e.committed_at != y->e.committed_at)
		return x->e.committed_at < y->e.committed_at ? -1 : 1;
	if (x->id.len != y->id.len)
		return x->id.len < y->id.len ? -1 : 1;
	return memcmp(x->id.bytes, y->id.bytes, x->id.len);
}

/*
 * A fragment's identity: the root of a tree of its commits' index entries
 * (keyed by date, then id; each its parents: not its height, which a commit
 * may not record), as any tree of
 * those entries is built.
 */
static void
fragment_root(List *severed, ProllyHash *root)
{
	ProllyTreeBuilder *b;
	ProllyNodeRef ref;
	StringInfoData k;
	StringInfoData v;
	List	   *sorted = list_copy(severed);
	ListCell   *lc;

	list_sort(sorted, entry_cmp);
	initStringInfo(&k);
	initStringInfo(&v);
	b = PROLLY_CALL(tree, build_begin, &tree_env, CurrentMemoryContext);
	foreach(lc, sorted)
	{
		PlanCommit *pc = lfirst(lc);
		ProllyKey	key;
		ProllyValue value;

		resetStringInfo(&k);
		resetStringInfo(&v);
		put_be64(&k, ((uint64) pc->e.committed_at) ^ ((uint64) 1 << 63));
		put_id(&k, &pc->id);
		appendStringInfoChar(&v, (char) pc->e.nparents);
		for (int i = 0; i < pc->e.nparents; i++)
			put_id(&v, &pc->e.parents[i]);
		key.data = (const uint8 *) k.data;
		key.len = k.len;
		value.data = (const uint8 *) v.data;
		value.len = v.len;
		PROLLY_CALL(tree, build_add, b, &key, &value);
	}
	PROLLY_CALL(tree, build_finish, b, &ref);
	*root = ref.hash;
	pfree(k.data);
	pfree(v.data);
	list_free(sorted);
}

static int64
next_number(void)
{
	char		n = KEY_NEXT;
	ProllyKey	key = {(const uint8 *) &n, 1};
	StringInfoData v;
	int64		next = 1;

	initStringInfo(&v);
	if (PROLLY_CALL(dc.refs, side_get, SPACE_HORIZON, &key, &v))
	{
		if (v.len != 8)
			horizon_corrupt();
		next = (int64) get_be64(v.data);
	}
	resetStringInfo(&v);
	put_be64(&v, (uint64) (next + 1));
	{
		ProllyValue value = {(const uint8 *) v.data, v.len};

		PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
	}
	pfree(v.data);
	return next;
}

/* Severing or repairing history is the database owner's (what, which). */
static void
check_may_sever(const char *what)
{
	if (!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to %s history of database \"%s\"",
						what, get_database_name(MyDatabaseId)),
				 errdetail("Only the database's owner or a superuser may.")));
}

/*
 * The archive's time to answer: prolly.history_archive_timeout (ten
 * minutes where unset) without progress, else the sever is given up,
 * nothing severed, so an archive that hangs doesn't hold the sever (nor,
 * in the background, a worker) for ever.  Progress is the archive taking
 * or giving a chunk, or answering a call: a large fragment that goes on
 * moving finishes, however long it takes (archive_remote()).
 */
#define DEFAULT_ARCHIVE_TIMEOUT	(10 * USECS_PER_MINUTE)

static int	archive_timeout_id = -1;
static int	archive_timeout_ms = 0;
static volatile sig_atomic_t archive_timed_out = false;

static int64
archive_timeout(void)
{
	int64		usec = database_span("prolly.history_archive_timeout");

	return usec > 0 ? usec : DEFAULT_ARCHIVE_TIMEOUT;
}

static void
archive_timeout_handler(void)
{
	archive_timed_out = true;
	InterruptPending = true;
	QueryCancelPending = true;
	SetLatch(MyLatch);
}

/* The time without progress restarts. */
static void
archive_progress(void)
{
	if (archive_timeout_id >= 0 && archive_timeout_ms > 0)
		enable_timeout_after(archive_timeout_id, archive_timeout_ms);
}

/*
 * The archive's routine as a sever calls it: each call, and each chunk a
 * call takes or gives, progress.
 */
static const ProllyRemoteRoutine *archive_inner;
static ProllyRemoteRoutine archive_routine;

typedef struct ProgressNext
{
	ProllyChunkNext next;
	void	   *arg;
} ProgressNext;

static bool
progress_next(void *arg, ProllyHash *hash, StringInfo data)
{
	ProgressNext *pn = arg;
	bool		more = pn->next(pn->arg, hash, data);

	archive_progress();
	return more;
}

typedef struct ProgressVisit
{
	ProllyRemoteChunkVisit visit;
	void	   *arg;
} ProgressVisit;

static void
progress_visit(const ProllyHash *hash, const char *data, int len, void *arg)
{
	ProgressVisit *pv = arg;

	archive_progress();
	pv->visit(hash, data, len, pv->arg);
}

static void
ar_format(ProllyRemote *r, StringInfo out)
{
	archive_inner->format(r, out);
	archive_progress();
}

static void
ar_heads(ProllyRemote *r, ProllyRemoteHeadVisit visit, void *arg)
{
	archive_inner->heads(r, visit, arg);
	archive_progress();
}

static bool
ar_get(ProllyRemote *r, const ProllyHash *hash, StringInfo out)
{
	bool		got = archive_inner->get(r, hash, out);

	archive_progress();
	return got;
}

static bool
ar_has(ProllyRemote *r, const ProllyHash *hash)
{
	bool		has = archive_inner->has(r, hash);

	archive_progress();
	return has;
}

static void
ar_add_chunks(ProllyRemote *r, ProllyChunkNext next, void *arg)
{
	ProgressNext pn = {next, arg};

	archive_inner->add_chunks(r, progress_next, &pn);
	archive_progress();
}

static bool
ar_move_head(ProllyRemote *r, const char *branch, const ProllyHash *expected,
			 const ProllyHash *head, const char *format, bool force)
{
	bool		moved = archive_inner->move_head(r, branch, expected, head,
												 format, force);

	archive_progress();
	return moved;
}

static void
ar_get_many(ProllyRemote *r, const ProllyHash *hashes, int n,
			ProllyRemoteChunkVisit visit, void *arg)
{
	ProgressVisit pv = {visit, arg};

	archive_inner->get_many(r, hashes, n, progress_visit, &pv);
	archive_progress();
}

static void
ar_has_many(ProllyRemote *r, const ProllyHash *hashes, int n, bool *has)
{
	archive_inner->has_many(r, hashes, n, has);
	archive_progress();
}

static void
ar_get_since(ProllyRemote *r, const ProllyHash *wanted, int nwanted,
			 const ProllyHash *known, int nknown,
			 ProllyRemoteChunkVisit visit, void *arg)
{
	ProgressVisit pv = {visit, arg};

	archive_inner->get_since(r, wanted, nwanted, known, nknown,
							 progress_visit, &pv);
	archive_progress();
}

static const ProllyRemoteRoutine *
archive_remote(const ProllyRemoteRoutine *rr)
{
	archive_inner = rr;
	archive_routine = *rr;
#define WRAP(f) archive_routine.f = rr->f ? ar_##f : NULL
	WRAP(format);
	WRAP(heads);
	WRAP(get);
	WRAP(has);
	WRAP(add_chunks);
	WRAP(move_head);
	WRAP(get_many);
	WRAP(has_many);
	WRAP(get_since);
#undef WRAP
	return &archive_routine;
}

/* Raise the error caught again, or, the archive's time up, say so. */
static void
archive_rethrow(const char *url, int64 limit)
{
	if (!archive_timed_out)
		PG_RE_THROW();
	archive_timed_out = false;
	FlushErrorState();
	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_FAILURE),
			 errmsg("the archive at %s made no progress for %lld seconds",
					url, (long long) (limit / USECS_PER_SEC)),
			 errdetail("Nothing is severed."),
			 errhint("Set prolly.history_archive_timeout for the database to wait longer.")));
}

typedef struct ReadBack
{
	const ProllyHashRoutine *hash;
	HTAB	   *wanted;			/* ProllyHash: not yet read back whole */
	HTAB	   *verified;		/* and those read back, this sever's */
	StringInfoData chunk;
} ReadBack;

static void
read_back(const ProllyHash *hash, const char *data, int len, void *arg)
{
	ReadBack   *rb = arg;
	ProllyHashCtx *ctx;
	ProllyHash	actual;
	ProllyHash	key;

	resetStringInfo(&rb->chunk);
	if (!prolly_chunk_unframe(data, len, &rb->chunk))
		return;
	ctx = PROLLY_CALL(rb->hash, begin, CurrentMemoryContext);
	PROLLY_CALL(rb->hash, update, ctx, (const uint8 *) rb->chunk.data,
				rb->chunk.len);
	PROLLY_CALL(rb->hash, finish, ctx, &actual);
	if (actual.len != hash->len || memcmp(actual.bytes, hash->bytes, hash->len) != 0)
		return;
	key = hash_key(hash);
	(void) hash_search(rb->wanted, &key, HASH_REMOVE, NULL);
	if (rb->verified)
		(void) set_add(rb->verified, hash);
}

/*
 * An archive's name for fragment f's top i: by the database (the
 * cluster's system identifier and the database's OID) and the fragment's
 * identity, so the same fragment has the same name however often it is
 * archived (a sever rolled back and made again), and two databases sharing
 * an archive name theirs apart.
 */
static char *
archive_head_name(const ProllyWindowFragment *f, int i)
{
	return psprintf("history/" UINT64_FORMAT "-%u/%s/%d",
					GetSystemIdentifier(), MyDatabaseId,
					prolly_hash_hex(&f->root), i);
}

typedef struct HeadAt
{
	const char *name;
	ProllyHash	commit;
	bool		found;
} HeadAt;

static void
note_head_at(const char *branch, const ProllyHash *commit, void *arg)
{
	HeadAt	   *ha = arg;

	if (strcmp(branch, ha->name) == 0)
	{
		ha->commit = *commit;
		ha->found = true;
	}
}

/*
 * Send fragment f's chunks to the archive at url (those it lacks), name
 * its tops heads of the archive's, then read every chunk back from it and
 * re-hash it, before the horizon is moved:
 * an archive that says it has a chunk it doesn't, or holds other bytes
 * under its hash, is found out, and nothing is severed.  Its tops are
 * heads of the archive's (archive_head_name()), so the archive names what
 * it keeps.  The archive is opened as the session's role, so its checks
 * (a file remote's pg_write_server_files) are the role's.  The count of
 * chunks sent.
 */
static int64
archive_fragment(const char *url, const ProllyWindowFragment *f,
				 List *chunk_list, HTAB *verified)
{
	int64		sent = 0;
	const ProllyRemoteRoutine *rr;
	ProllyRemote *remote = NULL;
	int64		limit = archive_timeout();

	archive_timed_out = false;
	if (archive_timeout_id < 0)
		archive_timeout_id = RegisterTimeout(USER_TIMEOUT, archive_timeout_handler);
	archive_timeout_ms = (int) Min(limit / 1000, INT_MAX);
	archive_progress();
	PG_TRY();
	{
		const ProllyRemoteRoutine *inner = prolly_remote_for_url(url);

		remote = PROLLY_CALL(inner, open, url, true);
		rr = archive_remote(inner);
		archive_progress();
	}
	PG_CATCH();
	{
		disable_timeout(archive_timeout_id, false);
		archive_rethrow(url, limit);
	}
	PG_END_TRY();

	PG_TRY();
	{
		int			n = list_length(chunk_list);
		ProllyHash *hashes = palloc_array(ProllyHash, Max(n, 1));
		int			i = 0;
		StringInfoData format;
		ReadBack	rb;

		sent = (int64) PROLLY_CALL(dc.transfer, send, rr, remote, f->tops,
								   f->ntops);
		/*
		 * (named first, so the archive keeps what it was sent, its own
		 * collections too, while it is read back)
		 */
		initStringInfo(&format);
		PROLLY_CALL(dc.canonicalizer, format, &format);
		for (int t = 0; t < f->ntops; t++)
		{
			HeadAt		ha = {archive_head_name(f, t)};
			ProllyHash	none = {0};

			/* (named already, by a sever of the same fragment rolled back) */
			PROLLY_CALL(rr, heads, remote, note_head_at, &ha);
			if (ha.found && ha.commit.len == f->tops[t].len &&
				memcmp(ha.commit.bytes, f->tops[t].bytes, ha.commit.len) == 0)
				continue;
			if (!PROLLY_CALL(rr, move_head, remote, ha.name,
							 ha.found ? &ha.commit : &none, &f->tops[t],
							 format.data, true))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not name the history severed in the archive at %s",
								url)));
		}
		rb.hash = prolly_content_hash();
		rb.verified = verified;
		rb.wanted = id_set("prolly archive read-back", sizeof(ProllyHash));
		initStringInfo(&rb.chunk);
		/* (what an earlier try of this sever read back isn't again) */
		foreach_ptr(ProllyHash, h, chunk_list)
		{
			if (verified && set_has(verified, h))
				continue;
			hashes[i++] = *h;
			(void) set_add(rb.wanted, h);
		}
		n = i;
		if (PROLLY_HAS(rr, get_many))
			PROLLY_CALL(rr, get_many, remote, hashes, n, read_back, &rb);
		else
			for (i = 0; i < n; i++)
			{
				StringInfoData frame;

				initStringInfo(&frame);
				if (PROLLY_CALL(rr, get, remote, &hashes[i], &frame))
					read_back(&hashes[i], frame.data, frame.len, &rb);
				pfree(frame.data);
			}
		if (hash_get_num_entries(rb.wanted) > 0)
		{
			HASH_SEQ_STATUS seq;
			ProllyHash *missing;

			hash_seq_init(&seq, rb.wanted);
			missing = hash_seq_search(&seq);
			hash_seq_term(&seq);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the archive at %s doesn't give back %ld of the %d chunks of the history severed",
							url, hash_get_num_entries(rb.wanted), n),
					 errdetail("Chunk %s, for one, isn't there, or isn't what its hash says.  Nothing is severed.",
							   prolly_hash_hex(missing))));
		}
	}
	PG_CATCH();
	{
		disable_timeout(archive_timeout_id, false);
		PROLLY_CALL(rr, close, remote);
		archive_rethrow(url, limit);
	}
	PG_END_TRY();
	disable_timeout(archive_timeout_id, false);
	PROLLY_CALL(rr, close, remote);
	return sent;
}

/* How many times a sever plans and archives again, the refs moved. */
#define SEVER_TRIES		3

/*
 * Fragment f of plan's severed commits: its dates, identity, digest and
 * chunks (chunk_list, if wanted), tops and stops.
 */
static void
build_fragment(Plan *plan, ProllyWindowAction action, bool want_chunks,
			   ProllyWindowFragment *f, List **ids, List **chunk_list,
			   int *nstops, ProllyHash **stops)
{
	ListCell   *lc;
	ProllyHash *tops;
	int			ntops = 0;
	int			i = 0;

	*ids = NIL;
	*chunk_list = NIL;
	memset(f, 0, sizeof(*f));
	f->deleted = (action == PROLLY_WINDOW_DELETE);
	f->severed_at = GetCurrentTimestamp();
	f->oldest = DT_NOEND;
	f->newest = DT_NOBEGIN;
	foreach(lc, plan->severed)
	{
		PlanCommit *pc = lfirst(lc);

		*ids = lappend(*ids, &pc->id);
		f->oldest = Min(f->oldest, pc->e.committed_at);
		f->newest = Max(f->newest, pc->e.committed_at);
	}
	f->commits = list_length(plan->severed);
	fragment_root(plan->severed, &f->root);
	fragment_closure(*ids, &f->digest, &f->chunks, &f->bytes,
					 want_chunks ? chunk_list : NULL);

	/* its tops: each parent the boundary keeps no more, once */
	tops = palloc_array(ProllyHash, Max(list_length(plan->edges), 1));
	{
		HTAB	   *seen = id_set("prolly fragment tops", sizeof(ProllyHash));

		foreach(lc, plan->edges)
		{
			ProllyHash *pair = lfirst(lc);

			if (set_add(seen, &pair[1]))
				tops[ntops++] = pair[1];
		}
		hash_destroy(seen);
	}
	f->ntops = ntops;
	f->tops = tops;
	*nstops = list_length(plan->stops);
	*stops = palloc_array(ProllyHash, Max(*nstops, 1));
	foreach(lc, plan->stops)
		(*stops)[i++] = *(ProllyHash *) lfirst(lc);
}

/* Whether two plans' boundary edges are the same. */
static bool
same_edges(Plan *a, Plan *b)
{
	HTAB	   *seen;
	bool		same = true;

	if (list_length(a->edges) != list_length(b->edges))
		return false;
	seen = id_set("prolly plan edges", 2 * sizeof(ProllyHash));
	foreach_ptr(ProllyHash, pair, a->edges)
		(void) set_add(seen, pair);
	foreach_ptr(ProllyHash, pair, b->edges)
		if (!set_has(seen, pair))
			same = false;
	hash_destroy(seen);
	return same;
}

static void
archive_sever(ProllyWindowAction action, const TimestampTz *before,
			  ProllyWindowResult *result)
{
	sever_cut(action, before, DT_NOBEGIN, result);
}

/*
 * A sever (as archive_sever()), each head's window cut no earlier than
 * size_cut (DT_NOBEGIN: no limit), its count kept whatever: what a size
 * limit severs.
 */
static void
sever_cut(ProllyWindowAction action, const TimestampTz *before,
		  TimestampTz size_cut, ProllyWindowResult *result)
{
	Plan		plan;
	ListCell   *lc;
	List	   *ids = NIL;
	ProllyWindowFragment f;
	ProllyHash *stops;
	int			nstops;
	StringInfoData k;
	StringInfoData v;

	List	   *chunk_list = NIL;
	HTAB	   *verified = id_set("prolly archive verified", sizeof(ProllyHash));
	char	   *archive_url = NULL;
	ProllyWindowPolicy policy;

	ProllyHash	planned;
	ProllyHash	held;

	window_init();
	memset(result, 0, sizeof(*result));
	result->action = action;
	if (action == PROLLY_WINDOW_NONE)
		return;
	check_may_sever("sever");
	PreventCommandIfReadOnly("severing history");
	archive_policy(NULL, &policy);

	/*
	 * Planned, and archived, holding only the severs' own lock: no ref (a
	 * plan takes a while, and commits on every branch would wait), nor the
	 * horizon (an archive takes longer, and collections would wait).
	 */
	sever_busy = false;
	if (!sever_slot_lock())
	{
		sever_busy = true;
		return;
	}
	sever_clock = GetCurrentTimestamp();
	for (int tries = 1;; tries++)
	{
		ProllyHash	now;

		PROLLY_CALL(tree_env.chunk_store, root, tree_env.chunks, &planned);
		make_plan(&plan, before, size_cut);
		if (plan.severed == NIL)
		{
			/* (nothing to sever, but commits dated ahead seen first) */
			if (plan.arrivals != NIL)
			{
				PROLLY_CALL(dc.refs, lock);
				record_arrivals(&plan);
			}
			return;
		}

		/* (archive mode with nowhere to archive to: nothing is severed) */
		if (action == PROLLY_WINDOW_ARCHIVE && archive_url == NULL)
		{
			if (policy.archive == NULL)
			{
				ereport(WARNING,
						(errmsg("history of database \"%s\" is past its window, but isn't severed: it has no archive",
								get_database_name(MyDatabaseId)),
						 errdetail("%d commits are past the window.",
								   list_length(plan.severed)),
						 errhint("Set prolly.history_archive for the database (ALTER DATABASE ... SET), or set prolly.history_window_action to delete or none.")));
				return;
			}
			archive_url = policy.archive;
		}
		build_fragment(&plan, action, archive_url != NULL, &f, &ids,
					   &chunk_list, &nstops, &stops);
		if (archive_url)
		{
			result->sent += archive_fragment(archive_url, &f, chunk_list,
											 verified);
			f.location = archive_url;
		}
		INJECTION_POINT("prolly-sever-planned", NULL);

		/*
		 * (the refs moved meanwhile: planned and archived again, still
		 * holding nothing, a few times; the same fragment's names in the
		 * archive are the same)
		 */
		PROLLY_CALL(tree_env.chunk_store, root, tree_env.chunks, &now);
		if (!same_id(&planned, &now) && tries < SEVER_TRIES)
			continue;
		break;
	}

	/*
	 * Then the horizon and the refs held, only to check the plan and
	 * record it: if the refs moved since it was made, planned again (a
	 * walk, not the fragment's chunks), and the fragment must be the one
	 * archived.
	 */
	horizon_lock(ExclusiveLock);
	PROLLY_CALL(dc.refs, lock);
	PROLLY_CALL(tree_env.chunk_store, root, tree_env.chunks, &held);
	if (!same_id(&planned, &held))
	{
		Plan		again;
		ProllyHash	root;

		make_plan(&again, before, size_cut);
		if (again.severed == NIL)
			return;
		fragment_root(again.severed, &root);
		if (!same_id(&root, &f.root) || !same_edges(&again, &plan))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("the history of database \"%s\" moved while it was severed",
							get_database_name(MyDatabaseId)),
					 errdetail("Its commits past the window changed %d times while it was planned and archived.  Nothing is severed.",
							   SEVER_TRIES),
					 errhint("Sever again.")));
		plan = again;
	}
	record_arrivals(&plan);
	f.number = next_number();
	initStringInfo(&k);
	initStringInfo(&v);
	number_key(&k, KEY_FRAGMENT, f.number);
	fragment_encode(&v, &f,
					PROLLY_HAS(tree_env.chunk_store, generation) ?
					PROLLY_CALL(tree_env.chunk_store, generation,
								tree_env.chunks) : 0,
					nstops, stops);
	{
		ProllyKey	key = {(const uint8 *) k.data, k.len};
		ProllyValue value = {(const uint8 *) v.data, v.len};

		PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
	}
	foreach(lc, plan.edges)
	{
		ProllyHash *pair = lfirst(lc);
		ProllyKey	key;
		ProllyValue value;

		resetStringInfo(&k);
		resetStringInfo(&v);
		appendStringInfoChar(&k, KEY_EDGE);
		put_id(&k, &pair[1]);
		put_id(&k, &pair[0]);
		put_be64(&v, (uint64) f.number);
		key.data = (const uint8 *) k.data;
		key.len = k.len;
		value.data = (const uint8 *) v.data;
		value.len = v.len;
		PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
	}

	horizon_changed();

	result->commits = f.commits;
	result->edges = list_length(plan.edges);
	result->chunks = f.chunks;
	result->bytes = f.bytes;
	result->fragment = f.number;
	{
		HTAB	   *seen = id_set("prolly boundary", sizeof(ProllyHash));

		result->boundary = palloc_array(ProllyHash,
										Max(list_length(plan.edges), 1));
		foreach(lc, plan.edges)
		{
			ProllyHash *pair = lfirst(lc);

			if (set_add(seen, &pair[0]))
				result->boundary[result->nboundary++] = pair[0];
		}
		hash_destroy(seen);
	}
	{
		StringInfoData boundary;

		initStringInfo(&boundary);
		foreach(lc, plan.edges)
		{
			ProllyHash *pair = lfirst(lc);

			appendStringInfo(&boundary, "%s%s", boundary.len > 0 ? ", " : "",
							 prolly_hash_hex(&pair[0]));
		}
		ereport(LOG,
				(errmsg("history of database \"%s\" severed (%s): %lld commits dated %s to %s, fragment %lld",
						get_database_name(MyDatabaseId),
						f.deleted ? "deleted" : "archived",
						(long long) f.commits, timestamptz_to_str(f.oldest),
						timestamptz_to_str(f.newest), (long long) f.number),
				 errdetail("The boundary is %s.  The fragment's commits reach %lld chunks (%lld bytes), those no kept commit reaches freed by the next collection.",
						   boundary.data, (long long) f.chunks,
						   (long long) f.bytes)));
		pfree(boundary.data);
	}
	pfree(k.data);
	pfree(v.data);
}

/* ------------------------------------------------------------------------
 * Background severing
 * ------------------------------------------------------------------------
 */

/* Background severing cuts at whole hours where the window names no batch. */
#define DEFAULT_BACKGROUND_BATCH	USECS_PER_HOUR

#define DEFAULT_COLLECT_INTERVAL	USECS_PER_HOUR
#define BACKOFF_FIRST		USECS_PER_MINUTE
#define BACKOFF_MOST		USECS_PER_DAY

/* The background's state, the database's settings in the refs. */
#define LAST_CUT_SETTING	"history_window.last_cut"	/* :branch, a branch's */
#define SIZE_CUT_SETTING	"history_window.size_cut"
#define SIZE_PENDING_SETTING "history_window.size_pending"
#define SIZE_RETRY_SETTING	"history_window.size_retry_at"
#define MEASURED_SETTING	"history_window.measured"
#define OVER_COUNT_SETTING	"history_window.over_count"
#define FULL_WANTED_SETTING	"history_window.full_wanted"
#define LAST_FULL_SETTING	"history_window.last_full"
#define FAILURES_SETTING	"history_window.failures"
#define RETRY_SETTING		"history_window.retry_at"
#define TRANSIENT_SETTING	"history_window.transient"

/* Failures that pass by themselves tried again at once, this many in a row. */
#define TRANSIENT_MOST		5

static void
size_part(const char *name, int nchunks, uint64 size, bool settled, void *arg)
{
	*(uint64 *) arg += size;
}

/* What the database's chunk store holds, its journal and table files. */
static uint64
store_bytes(void)
{
	uint64		bytes = 0;

	PROLLY_CALL(tree_env.chunk_store, segments, tree_env.chunks, false,
				size_part, &bytes);
	PROLLY_CALL(tree_env.chunk_store, table_files, tree_env.chunks,
				size_part, &bytes);
	return bytes;
}

static void
oldest_of(const char *name, const ProllyHash *head, void *arg)
{
	void	  **a = arg;
	HTAB	   *seen = a[0];
	TimestampTz *oldest = a[1];
	List	   *todo;

	if (head->len == 0 || !set_add(seen, head))
		return;
	todo = list_make1(memcpy(palloc(sizeof(ProllyHash)), head, sizeof(ProllyHash)));
	while (todo != NIL)
	{
		ProllyHash *id = llast(todo);
		ProllyCommitInfo info;

		todo = list_delete_last(todo);
		CHECK_FOR_INTERRUPTS();
		if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
			continue;
		*oldest = Min(*oldest, info.committed_at);
		for (int i = 0; i < info.nparents; i++)
			if (!set_has(archived_parents(), &info.parents[i]) &&
				set_add(seen, &info.parents[i]))
				todo = lappend(todo, memcpy(palloc(sizeof(ProllyHash)),
											&info.parents[i], sizeof(ProllyHash)));
	}
}

/* The oldest date of the history kept here, every branch's. */
static TimestampTz
oldest_kept(void)
{
	TimestampTz oldest = DT_NOEND;
	void	   *a[2];

	a[0] = id_set("prolly oldest commit", sizeof(ProllyHash));
	a[1] = &oldest;
	PROLLY_CALL(dc.refs, branches, oldest_of, a);
	hash_destroy(a[0]);
	return oldest;
}

/* A setting of the background's, a number (dflt if unset). */
static int64
config_int(const char *name, int64 dflt)
{
	StringInfoData v;
	int64		n = dflt;

	initStringInfo(&v);
	if (PROLLY_CALL(dc.refs, config_get, name, &v))
		n = strtoll(v.data, NULL, 10);
	pfree(v.data);
	return n;
}

/* Set it (unset: none), written only if it changes. */
static void
config_int_set(const char *name, int64 n, bool unset)
{
	StringInfoData v;
	bool		had;
	char		buf[32];

	initStringInfo(&v);
	had = PROLLY_CALL(dc.refs, config_get, name, &v);
	snprintf(buf, sizeof(buf), INT64_FORMAT, n);
	if (unset ? had : (!had || strcmp(v.data, buf) != 0))
		PROLLY_CALL(dc.refs, config_set, name, unset ? NULL : buf);
	pfree(v.data);
}

static char *
last_cut_key(const char *branch)
{
	return branch ? psprintf("%s:%s", LAST_CUT_SETTING, branch) :
		pstrdup(LAST_CUT_SETTING);
}

/* The least time between the background's full collections. */
static int64
collect_interval(void)
{
	int64		usec = database_span("prolly.history_collect_interval");

	return usec > 0 ? usec : DEFAULT_COLLECT_INTERVAL;
}

typedef struct LineageCut
{
	char	   *key;			/* its last cut's setting */
	TimestampTz cut;
} LineageCut;


/*
 * The cuts that moved since each was last tried: every branch's, by its
 * lineage's policy (its own warm, min_commits and cold_batch over the
 * database's), and the database's, for the other refs.  So a branch's own
 * window is severed in the background, at its own batch, with none for
 * the database.  *any: some lineage has a window.
 */
static List *
moved_cuts(TimestampTz now, bool *any)
{
	List	   *names = list_make1(NULL);
	List	   *moved = NIL;

	*any = false;
	PROLLY_CALL(dc.refs, branches, note_lineage, &names);
	foreach_ptr(char, name, names)
	{
		ProllyWindowPolicy p;
		int64		batch;
		TimestampTz cut;
		char	   *key;

		archive_policy(name, &p);
		if (p.keep_time == 0 && p.keep_commits == 0)
			continue;
		*any = true;
		batch = p.batch > 0 ? p.batch : DEFAULT_BACKGROUND_BATCH;
		cut = p.keep_time > 0 ? now - p.keep_time : now;
		cut -= ((cut % batch) + batch) % batch;
		key = last_cut_key(name);
		if (cut > config_int(key, DT_NOBEGIN))
		{
			LineageCut *lc = palloc(sizeof(LineageCut));

			lc->key = key;
			lc->cut = cut;
			moved = lappend(moved, lc);
		}
	}
	return moved;
}

/*
 * Over prolly.history_max_size: the cut a batch past the oldest history
 * kept (and past the last cut for size), if the store is over by
 * what the last full collection the background ran left (the chunks it
 * kept, the unlinks it deferred not counted: the store's files as its
 * manifest has them), and no sever for size has been made since.  So a
 * batch at most for each collection that ran and was measured, never more
 * because freeing lags; in delete mode, only once two measurements in a
 * row say so.  A measurement wanted (none yet, or the store's files over
 * though the last was under) asks for a full collection; DT_NOBEGIN: no
 * cut for size.
 */
static TimestampTz
size_cut_due(const ProllyWindowPolicy *db, TimestampTz now, bool *want_full)
{
	int64		measured;
	int64		batch = db->batch > 0 ? db->batch : DEFAULT_BACKGROUND_BATCH;
	TimestampTz cut;

	if (db->max_bytes <= 0)
		return DT_NOBEGIN;
	if (config_int(SIZE_PENDING_SETTING, 0) != 0 ||
		now < config_int(SIZE_RETRY_SETTING, DT_NOBEGIN))
		return DT_NOBEGIN;
	measured = config_int(MEASURED_SETTING, -1);
	if (measured < 0 || measured <= db->max_bytes)
	{
		if (store_bytes() > (uint64) db->max_bytes)
			*want_full = true;
		return DT_NOBEGIN;
	}
	if (config_int(OVER_COUNT_SETTING, 0) <
		(db->action == PROLLY_WINDOW_DELETE ? 2 : 1))
	{
		*want_full = true;
		return DT_NOBEGIN;
	}
	/* (a batch past the oldest history kept, never short of the last) */
	cut = oldest_kept();
	if (cut == DT_NOEND)
		return DT_NOBEGIN;
	cut = Max(cut, config_int(SIZE_CUT_SETTING, DT_NOBEGIN));
	return Min(cut + batch, now);
}

static void
maintain_as_owner(const ProllyWindowPolicy *db, List *moved,
				  TimestampTz size_cut, ProllyWindowResult *r)
{
	HeapTuple	tuple;
	Oid			owner;
	Oid			save_userid;
	int			save_sec;

	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	owner = ((Form_pg_database) GETSTRUCT(tuple))->datdba;
	ReleaseSysCache(tuple);

	GetUserIdAndSecContext(&save_userid, &save_sec);
	SetUserIdAndSecContext(owner, save_sec | SECURITY_RESTRICTED_OPERATION);
	PG_TRY();
	{
		/*
		 * (the cuts tried noted after, not before: a refs write takes the
		 * refs' lock to the transaction's end, and commits would wait on
		 * this worker through the archive's transfer)
		 */
		sever_in_background = true;
		sever_cut(db->action, NULL, size_cut, r);
		sever_in_background = false;
		if (!sever_busy)
			foreach_ptr(LineageCut, lc, moved)
				config_int_set(lc->key, lc->cut, false);
	}
	PG_FINALLY();
	{
		sever_in_background = false;
		SetUserIdAndSecContext(save_userid, save_sec);
	}
	PG_END_TRY();
}

/*
 * The history window severed in the background (autovacuum's visit), as
 * the database's owner, so an archive's checks are the owner's, as at
 * dolt_history_sever(): only once some lineage's cut, rounded down to its
 * batch, has moved past the last one tried, or the size limit says (see
 * size_cut_due()).  A try that fails is reported and retried after a
 * wait that doubles with each failure in a row (a minute, to a day), not
 * at every visit.  True if a fragment was made: what only it reached is a
 * full collection's (collect_due()), which several fragments share.
 */
static bool
archive_maintain(void)
{
	ProllyWindowPolicy db;
	ProllyWindowResult r;
	TimestampTz now = GetCurrentTimestamp();
	TimestampTz size_cut;
	List	   *moved;
	bool		any;
	bool		want_full = false;
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	int64		failures;

	window_init();
	archive_policy(NULL, &db);
	if (db.action == PROLLY_WINDOW_NONE)
		return false;
	if (now < config_int(RETRY_SETTING, DT_NOBEGIN))
		return false;
	moved = moved_cuts(now, &any);
	size_cut = size_cut_due(&db, now, &want_full);
	if (moved == NIL && size_cut == DT_NOBEGIN)
	{
		if (want_full)
			config_int_set(FULL_WANTED_SETTING, 1, false);
		return false;
	}

	memset(&r, 0, sizeof(r));
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		maintain_as_owner(&db, moved, size_cut, &r);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;
		int64		wait;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		/*
		 * (one that passes by itself, the history moved meanwhile, a
		 * cancel by a lock's waiter, a lock not got: tried again at the
		 * next visit, no wait, no failure counted)
		 */
		if ((edata->sqlerrcode == ERRCODE_T_R_SERIALIZATION_FAILURE ||
			 edata->sqlerrcode == ERRCODE_QUERY_CANCELED ||
			 edata->sqlerrcode == ERRCODE_LOCK_NOT_AVAILABLE ||
			 edata->sqlerrcode == ERRCODE_T_R_DEADLOCK_DETECTED) &&
			config_int(TRANSIENT_SETTING, 0) + 1 < TRANSIENT_MOST)
		{
			config_int_set(TRANSIENT_SETTING,
						   config_int(TRANSIENT_SETTING, 0) + 1, false);
			ereport(LOG,
					(errmsg("background severing of history of database \"%s\" given up for now: %s",
							get_database_name(MyDatabaseId), edata->message),
					 errhint("Tried again at the next visit.")));
			FreeErrorData(edata);
			return false;
		}
		failures = config_int(FAILURES_SETTING, 0) + 1;
		wait = BACKOFF_FIRST;
		for (int64 i = 1; i < failures && wait < BACKOFF_MOST; i++)
			wait *= 2;
		wait = Min(wait, BACKOFF_MOST);
		config_int_set(FAILURES_SETTING, failures, false);
		config_int_set(RETRY_SETTING, now + wait, false);
		ereport(WARNING,
				(errcode(edata->sqlerrcode),
				 errmsg("background severing of history of database \"%s\" failed: %s",
						get_database_name(MyDatabaseId), edata->message),
				 edata->detail ? errdetail_internal("%s", edata->detail) : 0,
				 errhint("Tried again in %lld seconds (failure %lld in a row).",
						 (long long) (wait / USECS_PER_SEC),
						 (long long) failures)));
		FreeErrorData(edata);
		return false;
	}
	PG_END_TRY();
	/* (another sever has the turn: this visit's cuts tried at the next) */
	if (sever_busy)
		return false;
	config_int_set(FAILURES_SETTING, 0, true);
	config_int_set(RETRY_SETTING, 0, true);
	config_int_set(TRANSIENT_SETTING, 0, true);
	if (r.fragment > 0 || want_full)
		config_int_set(FULL_WANTED_SETTING, 1, false);
	if (size_cut != DT_NOBEGIN)
	{
		/* (one batch for size per measurement, a fragment made or not) */
		if (r.fragment > 0)
		{
			config_int_set(SIZE_CUT_SETTING, size_cut, false);
			config_int_set(SIZE_PENDING_SETTING, 1, false);
		}
		else
			config_int_set(SIZE_RETRY_SETTING, now + collect_interval(), false);
		config_int_set(OVER_COUNT_SETTING, 0, false);
	}
	return r.fragment > 0;
}

/*
 * Whether the background's collection is a full one now: fragments made
 * (or a measurement asked for) since its last full one, which ended at
 * least prolly.history_collect_interval ago (an hour where unset): so
 * several fragments share one collection, and full collections never run
 * back to back.
 */
static bool
archive_collect_due(void)
{
	window_init();
	if (config_int(FULL_WANTED_SETTING, 0) == 0)
		return false;
	return GetCurrentTimestamp() >=
		config_int(LAST_FULL_SETTING, DT_NOBEGIN) + collect_interval();
}

/*
 * The background's full collection done: when, and the store as it left
 * it (measured for the size limit), the fragments it freed no longer
 * waiting.
 */
static void
archive_collected(void)
{
	ProllyWindowPolicy db;
	uint64		bytes;

	window_init();
	archive_policy(NULL, &db);
	bytes = store_bytes();
	config_int_set(LAST_FULL_SETTING, GetCurrentTimestamp(), false);
	config_int_set(MEASURED_SETTING, (int64) bytes, false);
	config_int_set(FULL_WANTED_SETTING, 0, true);
	config_int_set(SIZE_PENDING_SETTING, 0, true);
	if (db.max_bytes > 0 && bytes > (uint64) db.max_bytes)
		config_int_set(OVER_COUNT_SETTING,
					   config_int(OVER_COUNT_SETTING, 0) + 1, false);
	else
		config_int_set(OVER_COUNT_SETTING, 0, true);
}

/* ------------------------------------------------------------------------
 * Repair
 * ------------------------------------------------------------------------
 */

static bool
collect_record(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	List	  **records = arg;
	FragmentRecord *r;

	if (key->len != 9 || key->data[0] != KEY_FRAGMENT)
		return false;
	r = palloc(sizeof(FragmentRecord));
	fragment_decode((const char *) value->data, value->len,
					(int64) get_be64((const char *) key->data + 1), r);
	*records = lappend(*records, r);
	return true;
}

/* The horizon's fragments' records, newest first. */
static List *
fragment_records(void)
{
	StringInfoData k;
	ProllyKey	from;
	List	   *records = NIL;

	initStringInfo(&k);
	number_key(&k, KEY_FRAGMENT, PG_INT64_MAX);
	from.data = (const uint8 *) k.data;
	from.len = k.len;
	PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
				BackwardScanDirection, collect_record, &records);
	pfree(k.data);
	return records;
}

typedef struct EdgesOf
{
	int64		number;
	List	   *keys;			/* StringInfo: each edge's key */
} EdgesOf;

static bool
collect_edge_of(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	EdgesOf    *eo = arg;

	if (key->len < 1 || key->data[0] != KEY_EDGE)
		return false;
	if (value->len != 8)
		horizon_corrupt();
	if ((int64) get_be64((const char *) value->data) == eo->number)
	{
		StringInfo	k = makeStringInfo();

		appendBinaryStringInfo(k, (const char *) key->data, key->len);
		eo->keys = lappend(eo->keys, k);
	}
	return true;
}

/* A remote's name, if the refs know it, is its URL; else source is one. */
typedef struct NamedRemote
{
	const char *name;
	char	   *url;
} NamedRemote;

static void
match_remote(const char *name, const char *url, void *arg)
{
	NamedRemote *nr = arg;

	if (strcmp(name, nr->name) == 0)
		nr->url = pstrdup(url);
}

static char *
source_url(const char *source)
{
	NamedRemote nr = {source, NULL};

	PROLLY_CALL(dc.refs, remotes, match_remote, &nr);
	return nr.url ? nr.url : pstrdup(source);
}

/*
 * A fragment's commits, back in the store: from its tops down, to its
 * stops.  An error if one is missing still.
 */
static List *
fragment_commits(const FragmentRecord *r)
{
	HTAB	   *seen = id_set("prolly fragment commits", sizeof(ProllyHash));
	List	   *todo = NIL;
	List	   *commits = NIL;

	for (int i = 0; i < r->nstops; i++)
		(void) set_add(seen, &r->stops[i]);
	for (int i = 0; i < r->f.ntops; i++)
		if (set_add(seen, &r->f.tops[i]))
			todo = lappend(todo, (void *) &r->f.tops[i]);
	while (todo != NIL)
	{
		ProllyHash *id = llast(todo);
		ProllyCommitInfo info;
		PlanCommit *pc;

		todo = list_delete_last(todo);
		CHECK_FOR_INTERRUPTS();
		if (!PROLLY_CALL(tree_env.chunk_store, has, tree_env.chunks, id) ||
			!PROLLY_CALL(dc.refs, commit_get, id, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("fragment %lld of the history horizon isn't whole: commit %s is missing",
							(long long) r->f.number, prolly_hash_hex(id))));
		pc = palloc0(sizeof(PlanCommit));
		pc->id = *id;
		pc->e.committed_at = info.committed_at;
		pc->e.height = info.has_height ? info.height : 0;
		pc->e.nparents = info.nparents;
		for (int i = 0; i < info.nparents; i++)
		{
			pc->e.parents[i] = info.parents[i];
			if (set_add(seen, &info.parents[i]))
			{
				ProllyHash *p = palloc(sizeof(ProllyHash));

				*p = info.parents[i];
				todo = lappend(todo, p);
			}
		}
		commits = lappend(commits, pc);
	}
	hash_destroy(seen);
	return commits;
}

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/*
 * Repair the newest nfragments of the horizon's fragments (all, if < 0),
 * newest first: each one's chunks received from source (a remote's name
 * or URL; NULL: the fragment's archive), every one re-hashed as the
 * transfer receives it; then its commits, from its tops to its stops,
 * must give its identity and digest again, or nothing is repaired.  Its
 * edges and record are let go of: the history reads as it did.
 */
static int
archive_repair(const char *source, int nfragments)
{
	List	   *records;
	int			repaired = 0;

	List	   *chosen = NIL;
	List	   *urls = NIL;

	window_init();
	check_may_sever("repair");
	PreventCommandIfReadOnly("repairing history");

	/*
	 * Received first, holding the horizon's lock but not the refs (a
	 * transfer may take long; the horizon's lock blocks only collections
	 * and severs), each fragment down to its stops: an older fragment's
	 * tops are not received, so a source that has only this one serves
	 * it, and the horizon goes on cutting there.  The horizon's lock comes
	 * first, before the chunk store's that a receive takes to the end of
	 * the transaction, as a collection takes them.
	 */
	(void) sever_slot_lock();
	horizon_lock(ExclusiveLock);
	records = fragment_records();
	foreach_ptr(FragmentRecord, r, records)
	{
		char	   *url;
		const ProllyRemoteRoutine *rr;
		ProllyRemote *remote;

		if (nfragments >= 0 && list_length(chosen) >= nfragments)
			break;
		if (source == NULL && r->f.location == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("fragment %lld of the history horizon was deleted, not archived",
							(long long) r->f.number),
					 errhint("Name a remote or a clone that has its history.")));
		url = source ? source_url(source) : pstrdup(r->f.location);
		rr = prolly_remote_for_url(url);
		remote = PROLLY_CALL(rr, open, url, false);
		PG_TRY();
		{
			if (PROLLY_HAS(dc.transfer, receive_until))
				(void) PROLLY_CALL(dc.transfer, receive_until, rr, remote,
								   r->f.tops, r->f.ntops, r->stops, r->nstops);
			else
				(void) PROLLY_CALL(dc.transfer, receive, rr, remote,
								   r->f.tops, r->f.ntops);
		}
		PG_FINALLY();
		{
			PROLLY_CALL(rr, close, remote);
		}
		PG_END_TRY();
		chosen = lappend(chosen, r);
		urls = lappend(urls, url);
	}

	/* then verified, and let go of, holding the refs too */
	INJECTION_POINT("prolly-repair-received", NULL);
	PROLLY_CALL(dc.refs, lock);
	records = fragment_records();
	foreach_ptr(FragmentRecord, c, chosen)
	{
		FragmentRecord *r = NULL;
		char	   *url = list_nth(urls, foreach_current_index(c));
		List	   *commits;
		List	   *ids = NIL;
		ProllyHash	root;
		ProllyHash	digest;
		int64		chunks;
		int64		bytes;
		EdgesOf		eo = {c->f.number, NIL};
		char		e = KEY_EDGE;
		ProllyKey	from = {(const uint8 *) &e, 1};
		StringInfoData k;

		foreach_ptr(FragmentRecord, now, records)
			if (now->f.number == c->f.number)
				r = now;
		/* (one a repair let go of meanwhile is done) */
		if (r == NULL)
			continue;

		/* (whole again, and the same history: its identity and digest) */
		commits = fragment_commits(r);
		foreach_ptr(PlanCommit, pc, commits)
			ids = lappend(ids, &pc->id);
		fragment_root(commits, &root);
		fragment_closure(ids, &digest, &chunks, &bytes, NULL);
		if (list_length(commits) != r->f.commits || !same_hash(&root, &r->f.root) ||
			!same_hash(&digest, &r->f.digest))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("fragment %lld of the history horizon doesn't verify",
							(long long) r->f.number),
					 errdetail("Its %d commits from %s give identity %s and digest %s, not %s and %s.",
							   list_length(commits), url,
							   prolly_hash_hex(&root), prolly_hash_hex(&digest),
							   prolly_hash_hex(&r->f.root),
							   prolly_hash_hex(&r->f.digest))));

		/* (its edges and record let go of: the boundary keeps its parents) */
		PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
					ForwardScanDirection, collect_edge_of, &eo);
		foreach_ptr(StringInfoData, ek, eo.keys)
		{
			ProllyKey	key = {(const uint8 *) ek->data, ek->len};

			PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, NULL);
		}
		initStringInfo(&k);
		number_key(&k, KEY_FRAGMENT, r->f.number);
		{
			ProllyKey	key = {(const uint8 *) k.data, k.len};

			PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, NULL);
		}
		pfree(k.data);
		horizon_changed();
		repaired++;
		ereport(LOG,
				(errmsg("history of database \"%s\" repaired: fragment %lld, %lld commits dated %s to %s, from %s",
						get_database_name(MyDatabaseId), (long long) r->f.number,
						(long long) r->f.commits,
						timestamptz_to_str(r->f.oldest),
						timestamptz_to_str(r->f.newest), url)));
	}
	return repaired;
}

/* ------------------------------------------------------------------------
 * Readers at the horizon
 * ------------------------------------------------------------------------
 */

bool
prolly_window_archived(const ProllyHash *id)
{
	ProllyDatabaseComponents c;

	prolly_database_components(&c);
	if (c.history_window == NULL || !PROLLY_HAS(c.history_window, archived))
		return false;
	return PROLLY_CALL(c.history_window, archived, id);
}

void
prolly_window_check_missing(const ProllyHash *id)
{
	if (prolly_window_archived(id))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("commit %s is past the history horizon",
						prolly_hash_hex(id)),
				 errdetail("History before it was severed from this database: archived, or deleted."),
				 errhint("Run dolt_history_repair() to reattach it.")));
}

/* Does the horizon hold any fragment? */
static bool
horizon_any(void)
{
	return hash_get_num_entries(archived_parents()) > 0;
}

void
prolly_window_check_absent(const ProllyHash *id)
{
	ProllyDatabaseComponents c;

	prolly_window_check_missing(id);
	prolly_database_components(&c);
	if (c.history_window == NULL || c.history_window != &prolly_window_archive ||
		!IsTransactionState())
		return;
	window_init();
	if (horizon_any())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("commit %s isn't in this database: it may be past the history horizon",
						prolly_hash_hex(id)),
				 errdetail("History before the horizon was severed from this database: archived, or deleted."),
				 errhint("Run dolt_history_repair() to reattach it.")));
}

bool
prolly_window_stops_at(const ProllyHash *parent)
{
	static LocalTransactionId told = InvalidLocalTransactionId;

	if (!prolly_window_archived(parent))
		return false;
	if (told != MyProc->vxid.lxid)
	{
		told = MyProc->vxid.lxid;
		ereport(NOTICE,
				(errmsg("history stops at the history horizon"),
				 errdetail("Commit %s and the history before it were severed from this database.",
						   prolly_hash_hex(parent)),
				 errhint("Run dolt_history_repair() to reattach it.")));
	}
	return true;
}

const ProllyWindowRoutine prolly_window_archive = {
	.info = {
		.iface = PROLLY_IFACE_HISTORY_WINDOW,
		.impl_id = 1,
		.format_version = 1,
		.name = "archive",
		.description = "archive: history past a window severed at a horizon in the refs",
	},
	.plan = archive_plan,
	.sever = archive_sever,
	.archived = archive_archived,
	.edges = archive_edges,
	.fragments = archive_fragments,
	.hold = archive_hold,
	.repair = archive_repair,
	.policy = archive_policy,
	.set_policy = archive_set_policy,
	.maintain = archive_maintain,
	.collect_due = archive_collect_due,
	.collected = archive_collected,
};
