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
static void
sever_slot_lock(void)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, HORIZON_LOCK_CLASSID, 1, 0);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);
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
							   "The most history kept here (empty: no limit).",
							   "Read as the database has it.",
							   &max_size_setting, "", PGC_USERSET, 0,
							   check_size, NULL, NULL);
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

/* name's value as the database has it (its own, no role's); NULL: none. */
static char *
database_setting(const char *name)
{
	Relation	rel;
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	char	   *found = NULL;

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
			size_t		len = strlen(name);

			deconstruct_array_builtin(a, TEXTOID, &elems, NULL, &n);
			for (int i = 0; i < n; i++)
			{
				char	   *s = TextDatumGetCString(elems[i]);

				if (strncmp(s, name, len) == 0 && s[len] == '=')
					found = pstrdup(s + len + 1);
			}
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
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

	if (p->keep_time > 0 && hot > p->keep_time)
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
static bool
young(Plan *plan, PlanCommit *pc, TimestampTz cut)
{
	TimestampTz now = GetCurrentTimestamp();
	List	   *stack;

	if (pc->young_cut == cut && cut != DT_NOBEGIN)
		return pc->young;
	stack = list_make1(pc);
	while (stack != NIL)
	{
		PlanCommit *top = llast(stack);
		bool		pending = false;
		bool		result = Min(top->e.committed_at, now) >= cut;

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
head_windows(Plan *plan, const TimestampTz *before)
{
	TimestampTz now = GetCurrentTimestamp();
	bool		any = false;

	foreach_ptr(Head, h, plan->heads)
	{
		ProllyWindowPolicy p;

		if (before != NULL)
		{
			h->cut = *before;
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
	}
	return any;
}

/*
 * The plan (before: every head's window cut there; NULL: as the policy
 * has it): the window, the boundary edges from it, and the fragment
 * beyond them.  The caller holds every ref.
 */
static void
make_plan(Plan *plan, const TimestampTz *before)
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
	if (!head_windows(plan, before))
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

	window_init();
	/* (read as the refs stand: no ref is held, nothing written) */
	make_plan(&plan, before);
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
 * The archive's time to answer: what a sever sends it, and reads back,
 * within prolly.history_archive_timeout (ten minutes where unset), else
 * the sever is given up, nothing severed, so an archive that hangs doesn't
 * hold the sever (nor, in the background, a worker) for ever.
 */
#define DEFAULT_ARCHIVE_TIMEOUT	(10 * USECS_PER_MINUTE)

static int	archive_timeout_id = -1;
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
			 errmsg("the archive at %s didn't answer within %lld seconds",
					url, (long long) (limit / USECS_PER_SEC)),
			 errdetail("Nothing is severed."),
			 errhint("Set prolly.history_archive_timeout for the database to wait longer.")));
}

typedef struct ReadBack
{
	const ProllyHashRoutine *hash;
	HTAB	   *wanted;			/* ProllyHash: not yet read back whole */
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
				 List *chunk_list)
{
	int64		sent = 0;
	const ProllyRemoteRoutine *rr;
	ProllyRemote *remote = NULL;
	int64		limit = archive_timeout();

	archive_timed_out = false;
	if (archive_timeout_id < 0)
		archive_timeout_id = RegisterTimeout(USER_TIMEOUT, archive_timeout_handler);
	enable_timeout_after(archive_timeout_id, (int) Min(limit / 1000, INT_MAX));
	PG_TRY();
	{
		rr = prolly_remote_for_url(url);
		remote = PROLLY_CALL(rr, open, url, true);
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
		rb.wanted = id_set("prolly archive read-back", sizeof(ProllyHash));
		initStringInfo(&rb.chunk);
		foreach_ptr(ProllyHash, h, chunk_list)
		{
			hashes[i++] = *h;
			(void) set_add(rb.wanted, h);
		}
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
	Plan		plan;
	ListCell   *lc;
	List	   *ids = NIL;
	ProllyWindowFragment f;
	ProllyHash *stops;
	int			nstops;
	StringInfoData k;
	StringInfoData v;

	List	   *chunk_list = NIL;
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
	sever_slot_lock();
	for (int tries = 1;; tries++)
	{
		ProllyHash	now;

		PROLLY_CALL(tree_env.chunk_store, root, tree_env.chunks, &planned);
		make_plan(&plan, before);
		if (plan.severed == NIL)
			return;

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
			result->sent += archive_fragment(archive_url, &f, chunk_list);
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

		make_plan(&again, before);
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
	sever_slot_lock();
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
};
