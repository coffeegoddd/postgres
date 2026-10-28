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
 *	'c' commit			-> a commit severed: its fragment's number (8 bytes
 *						   big-endian), then its root and its dependencies'
 *						   (len 0: none), ids with their bytes
 *						   complemented.  Its commit object is kept: its
 *						   data is what is severed
 *	'p' commit			-> as 'c', for a severed commit whose data may be
 *						   here still: the commits a full collection must
 *						   not follow to their trees.  A sever lets go of
 *						   those whose trees are gone
 *	'r' tree commit		-> (empty) a severed commit's root or dependencies'
 *						   tree, and the commit, for the error a read of it
 *						   gives: several commits may share a tree
 *	'q' tree			-> (empty) a tree of a severed commit whose pending
 *						   record the last sever let go of: if the store
 *						   holds it again (written again since: a revert
 *						   to it, a repair's), a full collection doesn't
 *						   follow it from the severed commits 'r' names it
 *						   under, which would keep it for good.  The next
 *						   sever lets go of those still not held, so a
 *						   collection reads only the last sever's
 *	'D' boundary piece	-> what a boundary commit makes over its first
 *						   parent, severed (see fragment_closure()): a
 *						   piece (8 bytes big-endian) of it, each hash an
 *						   id with its bytes complemented; one boundary's
 *						   per sever, let go of when the parent is
 *						   repaired
 *
 * None names a chunk: a collection asks edges() which trees not to follow
 * from which commits (a severed commit's root and dependencies), and
 * keeps nothing for a fragment.
 *
 * What is kept.  From every commit the refs name (a branch's head, a
 * tag's, a remote-tracking branch's, a session's detached head, a
 * stash's, a merge's or rebase's in progress, a pin's, a kept commit's) a
 * walk keeps the commits its window does (dated at or after the cut), and
 * every merge base of a branch with each other ref keeps its data (by
 * itself: the search for it goes across the horizon, every commit object
 * kept), so merging never needs severed data.  The commits kept are the
 * window; an edge from one to a
 * parent outside it is a boundary edge, and the parent a top of the
 * fragment, which is every commit the tops reach outside the window and
 * the horizon there was already.
 *
 * A fragment's identity (its root) is the root hash of a tree of its
 * commits' index entries, keyed by date then id, built as any tree is:
 * the same commits give the same root wherever they are severed.  Its
 * digest is a hash over the sorted hashes of the chunks born in it (what
 * each of its commits makes that its first parent lacks), which repair
 * recomputes, from the same tops down to the same stops (the commits
 * outside the fragment its commits name), once the chunks are back.  A
 * sever and an archive cost what the fragment's commits changed, not the
 * database, but for the first fragment, whose oldest commit made its
 * whole tree.  A full collection still marks what is live to free it.
 *
 * Severing changes no commit and keeps every commit object: hashes stay,
 * and the history's graph (ids, parents, dates, messages) is whole, so
 * readers that list history (dolt_log), ancestry and the search for a
 * merge base go across the horizon as before.  Only the data is severed:
 * a severed commit's root and dependencies' trees.  One that reads them
 * (dolt_at, a diff, a merge's base's rows, a cherry-pick) finds them
 * missing once a collection freed them, and is told the commit is past
 * the horizon and how to repair it (prolly_window_check_root()).
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
#include "common/relpath.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
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
#define KEY_SEVERED		'c'
#define KEY_PENDING		'p'
#define KEY_ROOT		'r'
#define KEY_PRUNED		'q'

#define FRAGMENT_VERSION	3
#define FRAGMENT_DELETED	0x01

#define HORIZON_LOCK_CLASSID	PROLLY_HORIZON_LOCK_CLASSID

static void check_may_sever(const char *what);
static List *fragment_records(void);
static void sever_cut(ProllyWindowAction action, const TimestampTz *before,
					  TimestampTz size_cut, ProllyWindowResult *result);
static char *source_url(const char *source);
static int64 config_int(const char *name, int64 dflt);

/* The size limit's last cut, the background's (a setting in the refs). */
#define SIZE_CUT_SETTING	"history_window.size_cut"

/* ------------------------------------------------------------------------
 * Components and encodings
 * ------------------------------------------------------------------------
 */

static ProllyDatabaseComponents dc;

/* The newest generation holding a top of the last fragment made (0: none). */
static uint32 last_top_epoch = 0;
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
				int nstops, const ProllyHash *stops, const uint32 *top_epochs)
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
	/* (each top's generation in the chunk store, to repair it back to) */
	for (int i = 0; i < f->ntops; i++)
	{
		uint32		e = top_epochs ? top_epochs[i] : 0;

		for (int b = 3; b >= 0; b--)
			appendStringInfoChar(out, (char) (e >> (b * 8)));
	}
}

typedef struct FragmentRecord
{
	ProllyWindowFragment f;
	uint32		generation;
	int			nstops;
	ProllyHash *stops;
	uint32	   *top_epochs;		/* each top's chunk store generation, to
								 * put it back in (0: not known yet) */
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
	if (end - p != (ptrdiff_t) r->f.ntops * 4)
		horizon_corrupt();
	r->top_epochs = palloc0_array(uint32, Max(r->f.ntops, 1));
	for (int i = 0; i < r->f.ntops; i++)
	{
		r->top_epochs[i] = ((uint32) (uint8) p[0] << 24) |
			((uint32) (uint8) p[1] << 16) | ((uint32) (uint8) p[2] << 8) |
			(uint32) (uint8) p[3];
		p += 4;
	}
}

/* Write a fragment's record again (its tops' generations known since). */
static void
fragment_rewrite(const FragmentRecord *r)
{
	StringInfoData k;
	StringInfoData v;
	ProllyKey	key;
	ProllyValue value;

	initStringInfo(&k);
	initStringInfo(&v);
	appendStringInfoChar(&k, KEY_FRAGMENT);
	put_be64(&k, (uint64) r->f.number);
	fragment_encode(&v, &r->f, r->generation, r->nstops, r->stops,
					r->top_epochs);
	key.data = (const uint8 *) k.data;
	key.len = k.len;
	value.data = (const uint8 *) v.data;
	value.len = v.len;
	PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
	pfree(k.data);
	pfree(v.data);
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

/* A severed commit's entry, read back. */
typedef struct Severed
{
	ProllyHash	id;
	int64		number;
	ProllyHash	root;
	ProllyHash	dependencies;	/* len 0: none */
} Severed;

static bool
severed_decode(char tag, const ProllyKey *key, const ProllyValue *value,
			   Severed *sv)
{
	const char *p = (const char *) key->data;
	const char *end = p + key->len;

	if (p >= end || *p != tag)
		return false;
	(void) get_id(p + 1, end, &sv->id);
	p = (const char *) value->data;
	end = p + value->len;
	if (end - p < 8)
		horizon_corrupt();
	sv->number = (int64) get_be64(p);
	p = get_value_id(p + 8, end, &sv->root);
	(void) get_value_id(p, end, &sv->dependencies);
	return true;
}

typedef void (*SeveredVisit) (const Severed *sv, void *arg);

static bool
collect_severed(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	SeveredVisit visit = ((void **) arg)[0];
	Severed		sv;

	pgstat_count_prolly(PGSTAT_PROLLY_HORIZON_ENTRIES_READ, 1);
	if (!severed_decode(KEY_PENDING, key, value, &sv))
		return false;
	visit(&sv, ((void **) arg)[1]);
	return true;
}

/*
 * Visit the severed commits whose data may be here still (pending: not
 * yet found freed by a sever), as the refs at root have them (NULL:
 * now).  So a collection reads the commits whose trees it might yet
 * follow, not every commit ever severed.
 */
static void
visit_pending(const ProllyHash *root, ProllyChunkRead read, void *read_arg,
			  SeveredVisit visit, void *arg)
{
	char		c = KEY_PENDING;
	ProllyKey	from = {(const uint8 *) &c, 1};
	void	   *args[2] = {(void *) visit, arg};

	window_init();
	if (root != NULL)
		PROLLY_CALL(dc.refs, side_scan_at, root, read, read_arg,
					SPACE_HORIZON, &from, ForwardScanDirection,
					collect_severed, args);
	else
		PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
					ForwardScanDirection, collect_severed, args);
}

static void
put_entry(StringInfo k, StringInfo v)
{
	ProllyKey	key = {(const uint8 *) k->data, k->len};
	ProllyValue value = {(const uint8 *) v->data, v->len};

	PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, v ? &value : NULL);
}

static void
drop_entry(StringInfo k)
{
	ProllyKey	key = {(const uint8 *) k->data, k->len};

	PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, NULL);
}

/* Record commit id (info) severed, in fragment number. */
static void
severed_put(const ProllyHash *id, const ProllyCommitInfo *info, int64 number)
{
	StringInfoData k;
	StringInfoData v;
	ProllyHash	none = {0};

	initStringInfo(&k);
	initStringInfo(&v);
	appendStringInfoChar(&k, KEY_SEVERED);
	put_id(&k, id);
	put_be64(&v, (uint64) number);
	put_value_id(&v, &info->root.hash);
	put_value_id(&v, info->has_dependencies ? &info->dependencies.hash : &none);
	put_entry(&k, &v);
	/* (and pending: its data here until a sever finds it freed) */
	k.data[0] = KEY_PENDING;
	put_entry(&k, &v);
	resetStringInfo(&v);
	/* (a tree to each commit severed with it: several may share one) */
	for (int t = 0; t < (info->has_dependencies ? 2 : 1); t++)
	{
		resetStringInfo(&k);
		appendStringInfoChar(&k, KEY_ROOT);
		put_id(&k, t == 0 ? &info->root.hash : &info->dependencies.hash);
		put_id(&k, id);
		put_entry(&k, &v);
	}
	pfree(k.data);
	pfree(v.data);
}

/* Let go of commit id's record: its data is back. */
static void
severed_drop(const ProllyHash *id, const ProllyCommitInfo *info)
{
	StringInfoData k;

	initStringInfo(&k);
	appendStringInfoChar(&k, KEY_SEVERED);
	put_id(&k, id);
	drop_entry(&k);
	k.data[0] = KEY_PENDING;
	drop_entry(&k);
	for (int t = 0; t < (info->has_dependencies ? 2 : 1); t++)
	{
		resetStringInfo(&k);
		appendStringInfoChar(&k, KEY_ROOT);
		put_id(&k, t == 0 ? &info->root.hash : &info->dependencies.hash);
		put_id(&k, id);
		drop_entry(&k);
	}
	pfree(k.data);
}

/* Whether a commit's data is severed, as a point lookup kept for the transaction. */
typedef struct SeveredMemo
{
	ProllyHash	id;				/* key */
	bool		severed;
} SeveredMemo;

/*
 * Whether commit id's data is severed (its 'c' entry): looked up once a
 * transaction, each commit asked of, so a reader pays for the commits it
 * reads, not for every one ever severed.
 */
static bool
is_severed(const ProllyHash *id)
{
	ProllyHash	k = hash_key(id);
	SeveredMemo *m;
	bool		found;

	if (id->len == 0)
		return false;
	if (archived_set == NULL || archived_lxid != MyProc->vxid.lxid)
	{
		if (archived_cxt == NULL)
			archived_cxt = AllocSetContextCreate(TopMemoryContext,
												 "prolly history horizon",
												 ALLOCSET_SMALL_SIZES);
		MemoryContextReset(archived_cxt);
		{
			MemoryContext old = MemoryContextSwitchTo(archived_cxt);

			archived_set = id_set("prolly horizon commits", sizeof(SeveredMemo));
			MemoryContextSwitchTo(old);
		}
		archived_lxid = MyProc->vxid.lxid;
	}
	m = hash_search(archived_set, &k, HASH_ENTER, &found);
	if (!found)
	{
		StringInfoData key;
		StringInfoData v;
		ProllyKey	pk;

		m->severed = false;
		initStringInfo(&key);
		initStringInfo(&v);
		appendStringInfoChar(&key, KEY_SEVERED);
		put_id(&key, id);
		pk.data = (const uint8 *) key.data;
		pk.len = key.len;
		pgstat_count_prolly(PGSTAT_PROLLY_HORIZON_ENTRIES_READ, 1);
		m->severed = PROLLY_CALL(dc.refs, side_get, SPACE_HORIZON, &pk, &v);
		pfree(key.data);
		pfree(v.data);
	}
	return m->severed;
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
	return is_severed(id);
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

/* A severed commit's trees, not followed from it by a collection. */
static void
edge_of_severed(const Severed *sv, void *arg)
{
	ProllyWindowEdgeVisit visit = ((void **) arg)[0];
	void	   *varg = ((void **) arg)[1];

	visit(&sv->id, &sv->root, varg);
	if (sv->dependencies.len > 0)
		visit(&sv->id, &sv->dependencies, varg);
}

static void note_head_deps(const char *name, const ProllyHash *head, void *arg);

/* The trees of 'q' entries, or the commits of 'r' entries under a prefix. */
typedef struct PrunedScan
{
	StringInfoData prefix;		/* 'q', or 'r' and a tree */
	List	   *ids;			/* ProllyHash *: the trees, or the commits */
} PrunedScan;

static bool
collect_pruned(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	PrunedScan *ps = arg;
	ProllyHash *id;

	if (key->len <= (Size) ps->prefix.len ||
		memcmp(key->data, ps->prefix.data, ps->prefix.len) != 0)
		return false;
	pgstat_count_prolly(PGSTAT_PROLLY_HORIZON_ENTRIES_READ, 1);
	id = palloc(sizeof(ProllyHash));
	(void) get_id((const char *) key->data + ps->prefix.len,
				  (const char *) key->data + key->len, id);
	ps->ids = lappend(ps->ids, id);
	return true;
}

static void
scan_pruned(const ProllyHash *root, ProllyChunkRead read, void *read_arg,
			PrunedScan *ps)
{
	ProllyKey	from = {(const uint8 *) ps->prefix.data, ps->prefix.len};

	PROLLY_CALL(dc.refs, side_scan_at, root, read, read_arg, SPACE_HORIZON,
				&from, ForwardScanDirection, collect_pruned, ps);
}

/*
 * The trees of severed commits whose pending records were let go of
 * ('q'), each the store holds again: not followed from those commits
 * ('r'), as a pending record's aren't.  One read of each such tree, as
 * the collection has it, and the commits of those it holds.
 */
static void
edges_of_pruned(const ProllyHash *refs_root, ProllyChunkRead read,
				void *read_arg, ProllyWindowEdgeVisit visit, void *arg)
{
	PrunedScan	trees = {{0}, NIL};
	StringInfoData data;
	HTAB	   *live_deps = NULL;

	initStringInfo(&data);
	initStringInfo(&trees.prefix);
	appendStringInfoChar(&trees.prefix, KEY_PRUNED);
	scan_pruned(refs_root, read, read_arg, &trees);
	foreach_ptr(ProllyHash, tree, trees.ids)
	{
		PrunedScan	commits = {{0}, NIL};

		/*
		 * (a dependencies' tree a branch head shares is live by it, not
		 * to be cut from the many commits severed sharing it too)
		 */
		if (live_deps == NULL)
		{
			live_deps = id_set("prolly live dependencies", sizeof(ProllyHash));
			PROLLY_CALL(dc.refs, branches, note_head_deps, live_deps);
		}
		if (set_has(live_deps, tree))
			continue;

		/*
		 * (read as the collection has it, not asked of the store, whose
		 * answer would keep it for the collection running)
		 */
		resetStringInfo(&data);
		if (!read(read_arg, tree, &data))
			continue;
		initStringInfo(&commits.prefix);
		appendStringInfoChar(&commits.prefix, KEY_ROOT);
		put_id(&commits.prefix, tree);
		scan_pruned(refs_root, read, read_arg, &commits);
		foreach_ptr(ProllyHash, commit, commits.ids)
			visit(commit, tree, arg);
		list_free_deep(commits.ids);
		pfree(commits.prefix.data);
	}
	list_free_deep(trees.ids);
	pfree(trees.prefix.data);
	pfree(data.data);
	if (live_deps)
		hash_destroy(live_deps);
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
	{
		void	   *args[2] = {(void *) visit, arg};

		visit_pending(refs_root, read, read_arg, edge_of_severed, args);
	}
	edges_of_pruned(refs_root, read, read_arg, visit, arg);
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
static char *window_setting = NULL;
static int	min_commits_setting = 0;
static char *max_size_setting = NULL;
static char *root_max_size_setting = NULL;
static char *batch_setting = NULL;
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

/*
 * Every branch's history has a window: prolly.history_window can't be
 * unset, empty or nothing long.  Its default, a database that sets none
 * has.
 */
#define DEFAULT_WINDOW		"1 month"

static bool
check_window(char **newval, void **extra, GucSource source)
{
	int64		usec;

	if (*newval == NULL || **newval == '\0' || !parse_span(*newval, &usec) ||
		usec <= 0)
	{
		GUC_check_errdetail("Give a positive interval, such as \"1 month\": every branch's history has a window.");
		return false;
	}
	return true;
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
	DefineCustomStringVariable("prolly.history_window",
							   "How long each branch's history is kept here, its page roots too (past it, history is archived or deleted as prolly.history_window_action says, and roots let go of).",
							   "Read as the database has it (default 1 month); a branch may keep its own (dolt_history_window()), but not none.",
							   &window_setting, DEFAULT_WINDOW, PGC_USERSET, 0,
							   check_window, NULL, NULL);
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
	DefineCustomStringVariable("prolly.history_root_max_size",
							   "The most the page roots of every branch's commits may hold (empty: no limit).",
							   "Read as the database has it.  Past it, the oldest roots in the window are let go of first (never a tag's, nor a branch's newest); a move to a commit without one restores the nearest before it and writes the rows they differ by.",
							   &root_max_size_setting, "", PGC_USERSET, 0,
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
	DefineCustomStringVariable("prolly.history_batch",
							   "Severed history is cut at multiples of this (empty: anywhere), so fragments are whole hours or days.",
							   "Read as the database has it; a branch may keep its own.",
							   &batch_setting, "", PGC_USERSET, 0,
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

/* A size setting as the database has it (0: none, or not a size). */
int64
prolly_window_database_size(const char *name)
{
	char	   *v;
	int64		bytes = 0;

	window_init();
	v = database_setting(name);
	if (v == NULL || !parse_size(v, &bytes))
		return 0;
	return bytes;
}

static const char *const branch_settings[] = {"window", "min_commits",
"batch"};

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
	p->keep_time = database_span("prolly.history_window");
	if (p->keep_time <= 0)
		(void) parse_span(DEFAULT_WINDOW, &p->keep_time);
	p->batch = database_span("prolly.history_batch");
	if ((v = database_setting("prolly.history_min_commits")) != NULL)
		p->keep_commits = strtol(v, NULL, 10);
	if ((v = database_setting("prolly.history_max_size")) != NULL &&
		!parse_size(v, &p->max_bytes))
		p->max_bytes = 0;
	v = database_setting("prolly.history_archive");
	p->archive = (v && *v) ? v : NULL;
	if (name == NULL)
		return;
	if ((v = branch_setting(name, "window")) != NULL)
	{
		int64		own;

		if (parse_span(v, &own) && own > 0)
			p->keep_time = own;
	}
	if ((v = branch_setting(name, "batch")) != NULL)
		(void) parse_span(v, &p->batch);
	if ((v = branch_setting(name, "min_commits")) != NULL)
		p->keep_commits = strtol(v, NULL, 10);
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
				 errhint("A branch keeps its own window, min_commits or batch; the rest are the database's.")));
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
		else if (strcmp(setting, "window") == 0 && n <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("a branch's window must be a positive interval, not \"%s\"", value),
					 errhint("Every branch's history has a window; NULL gives the branch the database's again.")));
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
	bool		by_base;		/* kept only as a merge base, its data */
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
	TimestampTz cut;			/* and every commit dated at or after it */
	int64		min_commits;	/* and its newest this many */
	bool		standalone;		/* a tag's: kept itself, on a lineage too */
	bool		on_lineage;		/* another ref's, on a branch's history */
	int64		height_cut;		/* > 0: kept by height, not the cut's date */
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
	HTAB	   *heights;		/* of commits recording none */
	List	   *orphans;		/* ProllyHash *, pairs: a kept commit and a
								 * parent past the horizon no edge names it
								 * with (an amend of a boundary commit) */
	List	   *arrivals;		/* PlanCommit *: dated past now, first seen */
	List	   *bases;			/* ProllyHash *: merge bases, kept */
	List	   *ref_pins;		/* ProllyHash *: lineage refs' commits, kept */
	List	   *tops;			/* ProllyHash *: the fragment's, each once */
	HTAB	   *moved;			/* HeadArrival: the head logs' first moves */
	bool		quiet;			/* the window evaluated: no warnings */
} Plan;

static void
edge_key(StringInfo k, const ProllyHash *parent, const ProllyHash *child)
{
	appendStringInfoChar(k, KEY_EDGE);
	put_id(k, parent);
	put_id(k, child);
}

/* Whether the horizon has edge child -> parent. */
static bool
edge_known(const ProllyHash *child, const ProllyHash *parent)
{
	StringInfoData k;
	StringInfoData v;
	ProllyKey	key;
	bool		found;

	initStringInfo(&k);
	initStringInfo(&v);
	edge_key(&k, parent, child);
	key.data = (const uint8 *) k.data;
	key.len = k.len;
	found = PROLLY_CALL(dc.refs, side_get, SPACE_HORIZON, &key, &v);
	pfree(k.data);
	pfree(v.data);
	return found;
}

typedef struct ParentFragment
{
	StringInfoData prefix;
	int64		number;
} ParentFragment;

static bool
note_parent_fragment(const ProllyKey *key, const ProllyValue *value,
					 void *arg)
{
	ParentFragment *pf = arg;

	if (key->len < (Size) pf->prefix.len ||
		memcmp(key->data, pf->prefix.data, pf->prefix.len) != 0)
		return false;
	if (value->len == 8)
		pf->number = (int64) get_be64((const char *) value->data);
	return false;
}

/* The fragment a parent past the horizon is a top of (0: none). */
static int64
parent_fragment(const ProllyHash *parent)
{
	ParentFragment pf;
	ProllyKey	from;

	initStringInfo(&pf.prefix);
	appendStringInfoChar(&pf.prefix, KEY_EDGE);
	put_id(&pf.prefix, parent);
	pf.number = 0;
	from.data = (const uint8 *) pf.prefix.data;
	from.len = pf.prefix.len;
	PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
				ForwardScanDirection, note_parent_fragment, &pf);
	pfree(pf.prefix.data);
	return pf.number;
}

/*
 * Record each kept commit naming a parent past the horizon by no edge
 * (made since: an amend, a commit after a reset to a boundary commit), an
 * edge of the parent's fragment's: a collection then doesn't follow it,
 * and a repair of that fragment lets go of it.  The caller holds the
 * horizon and the refs.
 */
static bool
record_orphans(Plan *plan)
{
	bool		any = false;

	foreach_ptr(ProllyHash, pair, plan->orphans)
	{
		int64		n;
		StringInfoData k;
		StringInfoData v;
		ProllyKey	key;
		ProllyValue value;

		if (edge_known(&pair[0], &pair[1]) ||
			(n = parent_fragment(&pair[1])) == 0)
			continue;
		initStringInfo(&k);
		initStringInfo(&v);
		edge_key(&k, &pair[1], &pair[0]);
		put_be64(&v, (uint64) n);
		key.data = (const uint8 *) k.data;
		key.len = k.len;
		value.data = (const uint8 *) v.data;
		value.len = v.len;
		PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
		pfree(k.data);
		pfree(v.data);
		any = true;
	}
	return any;
}

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
pin_tracking(const char *remote, const char *branch, const ProllyHash *commit,
			 void *arg)
{
	(void) set_add((HTAB *) arg, commit);
}

static void
pin_pin(const char *name, const ProllyHash *commit, const ProllyValue *info,
		void *arg)
{
	(void) set_add((HTAB *) arg, commit);
}

static void
pin_stash(const char *name, int n, const char *branch, const ProllyHash *stash,
		  void *arg)
{
	ProllyCommitInfo info;

	(void) set_add((HTAB *) arg, stash);
	/* (a stash's commit names what it was made on) */
	if (PROLLY_CALL(dc.refs, commit_get, stash, &info))
		for (int i = 0; i < info.nparents; i++)
			(void) set_add((HTAB *) arg, &info.parents[i]);
}

/*
 * The commits a ref names for a reader, each of which keeps its own data
 * whatever the window: a remote-tracking branch's, a reader's pin's, a
 * stash's, and every branch's merge's or rebase's in progress, whichever
 * session began it.  (Not a branch's log, nor
 * the commits its pages are kept as of: those are its history, the
 * window's.  A tag keeps itself as a ref of its own.)
 */
static void
pin_in_progress(const char *branch, const ProllyHash *commit, void *arg)
{
	(void) set_add((HTAB *) arg, commit);
}

static HTAB *
ref_commits(void)
{
	HTAB	   *set = id_set("prolly ref commits", sizeof(ProllyHash));

	if (PROLLY_HAS(dc.refs, trackings))
		PROLLY_CALL(dc.refs, trackings, NULL, pin_tracking, set);
	if (PROLLY_HAS(dc.refs, pins))
		PROLLY_CALL(dc.refs, pins, pin_pin, set);
	if (PROLLY_HAS(dc.refs, stashes))
		PROLLY_CALL(dc.refs, stashes, pin_stash, set);
	if (PROLLY_HAS(dc.refs, in_progress))
		PROLLY_CALL(dc.refs, in_progress, pin_in_progress, set);
	return set;
}

typedef struct Gone
{
	List	   *gone;			/* Severed *, records to let go of */
	HTAB	   *live_deps;		/* the branch heads' dependencies' trees */
} Gone;

static void
note_head_deps(const char *name, const ProllyHash *head, void *arg)
{
	ProllyCommitInfo info;

	if (head->len > 0 && PROLLY_CALL(dc.refs, commit_get, head, &info) &&
		info.has_dependencies)
		(void) set_add((HTAB *) arg, &info.dependencies.hash);
}

static void
note_gone(const Severed *sv, void *arg)
{
	Gone	   *g = arg;

	if (PROLLY_CALL(tree_env.chunk_store, has, tree_env.chunks, &sv->root) ||
		(sv->dependencies.len > 0 &&
		 !set_has(g->live_deps, &sv->dependencies) &&
		 PROLLY_CALL(tree_env.chunk_store, has, tree_env.chunks,
					 &sv->dependencies)))
		return;
	g->gone = lappend(g->gone, memcpy(palloc(sizeof(Severed)), sv,
									  sizeof(Severed)));
}

/*
 * Let go of the 'q' entries an earlier sever made whose trees the store
 * still doesn't hold: a tree written again before now is held, and keeps
 * its entry.  The caller holds the refs.
 */
static void
drop_pruned_gone(void)
{
	PrunedScan	trees = {{0}, NIL};
	ProllyKey	from;
	StringInfoData k;

	initStringInfo(&trees.prefix);
	appendStringInfoChar(&trees.prefix, KEY_PRUNED);
	from.data = (const uint8 *) trees.prefix.data;
	from.len = trees.prefix.len;
	PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
				ForwardScanDirection, collect_pruned, &trees);
	initStringInfo(&k);
	foreach_ptr(ProllyHash, tree, trees.ids)
	{
		if (PROLLY_CALL(tree_env.chunk_store, has, tree_env.chunks, tree))
			continue;
		resetStringInfo(&k);
		appendStringInfoChar(&k, KEY_PRUNED);
		put_id(&k, tree);
		drop_entry(&k);
	}
	list_free_deep(trees.ids);
	pfree(trees.prefix.data);
	pfree(k.data);
}

/*
 * Let go of the pending records of severed commits whose trees a
 * collection has freed: no collection need cut them any more (a tree
 * written again is live by what wrote it, and the commits it was let go
 * of from are cut from it by its 'q' entry).  So what a collection reads
 * of the horizon is what it may yet free, and the trees the last sever let
 * go of the records of.  A dependencies' tree a branch head
 * shares is live, so it holds no record back; one it stops sharing later
 * stays, named by the severed commits let go of (a small tree, one for
 * each set of dependencies, until a repair or the commits' own severing
 * past the graph).  The caller holds the refs.
 */
static void
prune_pending(void)
{
	Gone		g = {NIL, id_set("prolly live dependencies", sizeof(ProllyHash))};
	List	   *gone;
	StringInfoData k;
	StringInfoData none;

	drop_pruned_gone();
	PROLLY_CALL(dc.refs, branches, note_head_deps, g.live_deps);
	visit_pending(NULL, NULL, NULL, note_gone, &g);
	gone = g.gone;
	hash_destroy(g.live_deps);
	initStringInfo(&k);
	initStringInfo(&none);
	foreach_ptr(Severed, sv, gone)
	{
		resetStringInfo(&k);
		appendStringInfoChar(&k, KEY_PENDING);
		put_id(&k, &sv->id);
		drop_entry(&k);
		/* (its trees, should the store hold one again: see 'q') */
		for (int t = 0; t < (sv->dependencies.len > 0 ? 2 : 1); t++)
		{
			resetStringInfo(&k);
			appendStringInfoChar(&k, KEY_PRUNED);
			put_id(&k, t == 0 ? &sv->root : &sv->dependencies);
			put_entry(&k, &none);
		}
	}
	pfree(k.data);
	pfree(none.data);
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
	 * window's to keep or cut, and keep no data of their own: they are let
	 * go of as the branch moves on, forget_kept(), not named for a reader)
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
 * data of every merge base of the two (every maximal common ancestor,
 * where histories cross): a merge into a branch is from another branch, a
 * tag, a remote-tracking branch or any ref's commit, so every pair of a
 * branch and a ref is one.  (Two refs that aren't branches merge only into
 * a branch, which pairs with each.)  Only the bases themselves: every
 * commit object is kept, so the search for a base goes across the
 * horizon, and a merge reads only the base's rows.
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
				plan->bases = lappend(plan->bases,
									  memcpy(palloc(sizeof(ProllyHash)),
											 &bases[k], sizeof(ProllyHash)));
		}
	}

	/*
	 * A ref's commit on a branch's history, but a tag's, has no window of
	 * its own: the commits a branch's pages are kept as of, a reader's pin,
	 * a remote-tracking branch are the branch's lineage, its window's to
	 * keep or cut.  A reader's (a remote-tracking branch's, a pin's, a
	 * stash's, a merge's or rebase's in progress: ref_commits()) keeps its
	 * own commit's data still, as a tag does (a collection follows no
	 * severed commit to its data, so only that keeps it): only the
	 * history behind it is the window's.
	 */
	{
		HASH_SEQ_STATUS seq;
		ProllyHash *r;
		HTAB	   *readers = ref_commits();

		foreach_ptr(Head, h, plan->heads)
			if (h->on_lineage && !h->standalone)
				plan->heads = foreach_delete_current(plan->heads, h);
		/*
		 * (each kept by itself, whether the refs' entries name it as a
		 * chunk or not: a merge's state names its commits by id)
		 */
		hash_seq_init(&seq, readers);
		while ((r = hash_seq_search(&seq)) != NULL)
			plan->ref_pins = lappend(plan->ref_pins,
									 memcpy(palloc(sizeof(ProllyHash)), r,
											sizeof(ProllyHash)));
		hash_destroy(readers);
	}
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
		if (!plan->quiet)
			ereport(WARNING,
					(errmsg("a commit is dated %s, after it arrived here",
							timestamptz_to_str(pc->e.committed_at)),
					 errdetail("Commit %s ages from %s instead.",
							   prolly_hash_hex(&pc->id),
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

			if (is_severed(&top->e.parents[i]))
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

static void
add_pair(List **list, const ProllyHash *a, const ProllyHash *b)
{
	ProllyHash *pair = palloc_array(ProllyHash, 2);

	pair[0] = *a;
	pair[1] = *b;
	*list = lappend(*list, pair);
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
		if (same_id(id, &h->head) ||
			(h->height_cut > 0 ? pc->e.height >= h->height_cut :
			 young(plan, pc, h->cut)) ||
			(h->min_commits > 0 && head_height - pc->e.height < h->min_commits))
		{
			if (pc->kept && pc->by_base)
				pc->by_base = false;
		}
		else
			continue;
		keep(plan, pc);
		for (int i = 0; i < pc->e.nparents; i++)
		{
			ProllyHash *p = &pc->e.parents[i];

			if (is_severed(p))
			{
				if (!edge_known(&pc->id, p))
					add_pair(&plan->orphans, &pc->id, p);
				continue;
			}
			if (!set_add(seen, p))
				continue;
			todo = lappend(todo, p);
		}
	}
	hash_destroy(seen);
}


/*
 * Each head's window: its commits dated before before (if given) cut;
 * else its lineage's policy's, as of now, the cut rounded down to a whole
 * batch.  False if no head's window cuts anything.
 */
static bool
head_windows(Plan *plan, const TimestampTz *before, TimestampTz size_cut,
			 int64 size_height)
{
	TimestampTz now = plan_now();
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
		/* (every lineage has a window: archive_policy()) */
		h->cut = now - p.keep_time;
		if (p.batch > 0)
			h->cut -= ((h->cut % p.batch) + p.batch) % p.batch;
		any = true;
		/* (a size limit cuts later, the count still keeping its own) */
		if (size_cut > h->cut)
		{
			h->cut = size_cut;
			any = true;
		}
		/* (or by height, where the size limit's dates cut nothing) */
		if (size_height > 0)
		{
			h->height_cut = size_height;
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
make_plan(Plan *plan, const TimestampTz *before, TimestampTz size_cut,
		  int64 size_height)
{
	ListCell   *lc;
	HTAB	   *fragment;
	List	   *todo = NIL;

	memset(plan, 0, sizeof(*plan));
	plan->commits = id_set("prolly window plan", sizeof(PlanCommit));
	plan->head_ids = id_set("prolly window heads", sizeof(ProllyHash));
	PROLLY_CALL(dc.refs, branches, note_branch, plan);
	note_refs(plan);
	if (!head_windows(plan, before, size_cut, size_height))
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

	/*
	 * (each merge base its data kept, by itself: the commit objects are
	 * all kept, so a search for it goes across the horizon, and only what
	 * a merge reads of it, its rows, need be here)
	 */
	foreach_ptr(ProllyHash, b, plan->bases)
	{
		PlanCommit *pc = plan_commit(plan, b);

		if (is_severed(b) || pc->kept)
			continue;
		pc->by_base = true;
		keep(plan, pc);
	}
	/* (and each commit a ref names on a branch's history, by itself) */
	foreach_ptr(ProllyHash, r, plan->ref_pins)
	{
		PlanCommit *pc;
		ProllyCommitInfo info;

		if (r->len == 0 || is_severed(r) ||
			!PROLLY_CALL(dc.refs, commit_get, r, &info))
			continue;
		pc = plan_commit(plan, r);
		if (pc->kept)
			continue;
		pc->pinned = true;
		keep(plan, pc);
	}

	/* the boundary: each parent of a kept commit that isn't kept */
	foreach(lc, plan->kept)
	{
		PlanCommit *pc = lfirst(lc);

		for (int i = 0; i < pc->e.nparents; i++)
		{
			ProllyHash *p = &pc->e.parents[i];
			PlanCommit *parent;

			if (is_severed(p))
				continue;
			parent = plan_commit(plan, p);
			if (!parent->kept)
			{
				add_pair(&plan->edges, &pc->id, p);
				todo = lappend(todo, p);
			}
		}
	}

	/*
	 * (and a commit beneath the horizon whose data a merge base or a ref
	 * kept, a fragment's stop, that nothing keeps now: no walk from a head
	 * reaches it past the severed commits above it, so it is a top too;
	 * it was older than its window when they were severed)
	 */
	foreach_ptr(FragmentRecord, r, fragment_records())
		for (int i = 0; i < r->nstops; i++)
		{
			ProllyHash *st = &r->stops[i];

			if (is_severed(st) || plan_commit(plan, st)->kept)
				continue;
			todo = lappend(todo, memcpy(palloc(sizeof(ProllyHash)), st,
										sizeof(ProllyHash)));
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
		plan->tops = list_copy(todo);
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
				if (is_severed(p) || plan_commit(plan, p)->kept)
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

/*
 * The commits in the window now (branch: its lineage's only), as a sever
 * would keep them for their age or count: each lineage's head walked by
 * its policy, with no other ref's keeping and no merge base's.
 * Quietly (no warning of a commit dated ahead: plan() and sever() give
 * that), and kept for the transaction while the refs stand as they did,
 * so a caller asking of each commit pays one walk: every lineage's window
 * and the last branch's are kept apart, so a caller asking of both (root
 * retention: a branch's own and every lineage's) walks each once.
 */
typedef struct WindowCache
{
	MemoryContext cxt;
	HTAB	   *in;
	char	   *branch;
	ProllyHash	root;
	LocalTransactionId lxid;
} WindowCache;

static WindowCache window_caches[2];	/* every lineage's, a branch's */

static HTAB *
window_commits(const char *branch)
{
	Plan		plan;
	HTAB	   *in;
	ProllyHash	root;
	MemoryContext old;
	ProllyWindowPolicy db;

	WindowCache *wc = &window_caches[branch == NULL ? 0 : 1];

	window_init();
	PROLLY_CALL(tree_env.chunk_store, root, tree_env.chunks, &root);
	if (wc->in != NULL && wc->lxid == MyProc->vxid.lxid &&
		same_id(&root, &wc->root) &&
		(branch == NULL || strcmp(branch, wc->branch) == 0))
		return wc->in;
	if (wc->cxt == NULL)
		wc->cxt = AllocSetContextCreate(TopMemoryContext,
										"prolly window evaluated",
										ALLOCSET_DEFAULT_SIZES);
	wc->in = NULL;
	MemoryContextReset(wc->cxt);
	old = MemoryContextSwitchTo(wc->cxt);
	in = id_set("prolly window", sizeof(ProllyHash));
	wc->branch = branch ? pstrdup(branch) : NULL;
	MemoryContextSwitchTo(old);
	sever_clock = DT_NOBEGIN;
	memset(&plan, 0, sizeof(plan));
	plan.quiet = true;
	plan.commits = id_set("prolly window plan", sizeof(PlanCommit));
	plan.head_ids = id_set("prolly window heads", sizeof(ProllyHash));
	PROLLY_CALL(dc.refs, branches, note_branch, &plan);
	note_refs(&plan);
	/*
	 * (the cut taken once, now, for every head; under a size limit, no
	 * earlier than the last the background cut for size)
	 */
	archive_policy(NULL, &db);
	(void) head_windows(&plan, NULL,
						db.max_bytes > 0 ?
						config_int(SIZE_CUT_SETTING, DT_NOBEGIN) : DT_NOBEGIN,
						0);
	foreach_ptr(Head, h, plan.heads)
	{
		if (h->name == NULL || (branch != NULL && strcmp(h->name, branch) != 0))
			continue;
		walk_window(&plan, h);
	}
	foreach_ptr(PlanCommit, pc, plan.kept)
		if (!pc->by_base)
			(void) set_add(in, &pc->id);
	hash_destroy(plan.commits);
	hash_destroy(plan.head_ids);
	wc->in = in;
	wc->root = root;
	wc->lxid = MyProc->vxid.lxid;
	return in;
}

static void
archive_window(const char *branch, ProllyWindowCommitVisit visit, void *arg)
{
	HTAB	   *in = window_commits(branch);
	HASH_SEQ_STATUS seq;
	ProllyHash *id;

	hash_seq_init(&seq, in);
	while ((id = hash_seq_search(&seq)) != NULL)
		if (!visit(id, arg))
		{
			hash_seq_term(&seq);
			break;
		}
}

static bool
archive_in_window(const char *branch, const ProllyHash *commit)
{
	return set_has(window_commits(branch), commit);
}

static void
archive_plan(const TimestampTz *before, ProllyWindowItemVisit visit, void *arg)
{
	Plan		plan;
	ListCell   *lc;

	sever_clock = DT_NOBEGIN;
	window_init();
	/* (read as the refs stand: no ref is held, nothing written) */
	make_plan(&plan, before, DT_NOBEGIN, 0);
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

/* A commit's delta, to keep: what it makes over its first parent. */
typedef struct Delta
{
	ProllyHash	id;
	List	   *hashes;			/* ProllyHash * */
} Delta;

typedef struct Closure
{
	bool		prune;			/* every walk whole: decline what is noted */
	HTAB	   *seen;
	List	   *chunks;			/* ProllyHash * */
	int64		bytes;
	StringInfoData buf;
} Closure;

static int
hash_cmp(const ListCell *a, const ListCell *b)
{
	const ProllyHash *x = lfirst(a);
	const ProllyHash *y = lfirst(b);

	if (x->len != y->len)
		return x->len < y->len ? -1 : 1;
	return memcmp(x->bytes, y->bytes, x->len);
}

#define KEY_DELTA		'D'
#define DELTA_PIECE		128		/* hashes a delta entry holds */

/* A node a commit makes, noted (once, however often it is visited). */
static bool
note_node(Closure *c, const ProllyHash *h)
{
	if (!set_add(c->seen, h))
		return false;
	c->chunks = lappend(c->chunks, memcpy(palloc(sizeof(ProllyHash)), h,
										  sizeof(ProllyHash)));
	resetStringInfo(&c->buf);
	if (PROLLY_CALL(tree_env.chunk_store, get, tree_env.chunks, h, &c->buf))
		c->bytes += c->buf.len;
	return true;
}

/*
 * Every visit descends, a node noted already too: a diff visits a node
 * and goes down only into the children that differ from the parent's, so
 * a node noted there may have children noted nowhere yet, which a later
 * commit's whole walk of it (a table made, whose root a revert brings
 * back) must reach.  Declining it would make what a commit adds depend on
 * the commits noted before it; descending, each adds exactly what it
 * makes, whatever the order, so the set found walking the commits is the
 * one found from a boundary's kept delta.  (A closure whose every walk is
 * whole, prune, declines what it noted: all below it is noted too.)
 */
static bool
closure_visit(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Closure    *c = arg;

	return note_node(c, &node->hash) || !c->prune;
}

/*
 * What commit c (info) makes that its first parent (pinfo; NULL: it has
 * none, or none here) lacks: its own tree's nodes, then its root's and its
 * dependencies' as they differ from the parent's (the canonicalizer's
 * walk_changes(), the tree's diff_nodes()), or all of them for a commit
 * with no parent.  The nodes born at c, by its history: whatever the store
 * held, a node c has and its parent lacks is c's, so what is resurrected
 * or deduplicated counts, and the set is a function of the two commits
 * alone.  (A merge's is against its first parent: more, never less.)
 */
static void
commit_created(Closure *c, const ProllyHash *id, const ProllyCommitInfo *info,
			   const ProllyCommitInfo *pinfo)
{
	/* (its commit object kept, what it makes is its data) */
	PROLLY_CALL(dc.canonicalizer, walk_changes, pinfo ? &pinfo->root : NULL,
				&info->root, closure_visit, c);
	if (info->has_dependencies)
		PROLLY_CALL(tree, diff_nodes, &tree_env,
					pinfo && pinfo->has_dependencies ? &pinfo->dependencies : NULL,
					&info->dependencies, closure_visit, c);
}

static void
delta_key(StringInfo k, const ProllyHash *id, uint32 piece)
{
	appendStringInfoChar(k, KEY_DELTA);
	put_id(k, id);
	put_be64(k, piece);
}

/*
 * Keep what boundary commit b makes that its parent (pinfo) lacks, beside
 * the horizon: its parent is severed now, and gone once collected, so a
 * later sever, or a repair's check, of the fragment b falls in finds b's
 * born nodes here.  Kept in pieces, each hash complemented.
 */
/* Keep hashes, what boundary commit b makes over its first parent. */
static void
write_delta(const ProllyHash *b, List *hashes)
{
	StringInfoData k;
	StringInfoData v;
	uint32		piece = 0;
	int			n = 0;

	initStringInfo(&k);
	initStringInfo(&v);
	foreach_ptr(ProllyHash, h, hashes)
	{
		put_value_id(&v, h);
		if (++n == DELTA_PIECE || foreach_current_index(h) == list_length(hashes) - 1)
		{
			ProllyKey	key;
			ProllyValue value;

			resetStringInfo(&k);
			delta_key(&k, b, piece++);
			key.data = (const uint8 *) k.data;
			key.len = k.len;
			value.data = (const uint8 *) v.data;
			value.len = v.len;
			PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
			resetStringInfo(&v);
			n = 0;
		}
	}
	pfree(k.data);
	pfree(v.data);
}

/*
 * Keep what boundary commit b makes that its parent (pinfo; NULL: it is
 * gone) lacks, beside the horizon: its parent is severed now, and gone
 * once collected, so a later sever, or a repair's check, of the fragment b
 * falls in finds b's born nodes here.  Kept in pieces, each hash
 * complemented.
 */
static void
keep_delta(const ProllyHash *b, const ProllyCommitInfo *binfo,
		   const ProllyCommitInfo *pinfo)
{
	Closure		c = {0};

	c.seen = id_set("prolly boundary delta", sizeof(ProllyHash));
	initStringInfo(&c.buf);
	commit_created(&c, b, binfo, pinfo);
	write_delta(b, c.chunks);
	pfree(c.buf.data);
	hash_destroy(c.seen);
}

/* Let go of boundary commit b's kept delta. */
static void
drop_delta(const ProllyHash *b)
{
	for (uint32 piece = 0;; piece++)
	{
		StringInfoData k;
		StringInfoData v;
		ProllyKey	key;
		bool		found;

		initStringInfo(&k);
		initStringInfo(&v);
		delta_key(&k, b, piece);
		key.data = (const uint8 *) k.data;
		key.len = k.len;
		found = PROLLY_CALL(dc.refs, side_get, SPACE_HORIZON, &key, &v);
		if (found)
			PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, NULL);
		pfree(k.data);
		pfree(v.data);
		if (!found)
			break;
	}
}

/* A boundary commit's kept delta, noted into c; false if none is kept. */
static bool
kept_delta(Closure *c, const ProllyHash *id)
{
	bool		found = false;

	for (uint32 piece = 0;; piece++)
	{
		StringInfoData k;
		StringInfoData v;
		ProllyKey	key;
		const char *p;
		const char *end;

		initStringInfo(&k);
		initStringInfo(&v);
		delta_key(&k, id, piece);
		key.data = (const uint8 *) k.data;
		key.len = k.len;
		if (!PROLLY_CALL(dc.refs, side_get, SPACE_HORIZON, &key, &v))
			break;
		found = true;
		p = v.data;
		end = v.data + v.len;
		while (p < end)
		{
			ProllyHash	h;

			p = get_value_id(p, end, &h);
			(void) note_node(c, &h);
		}
		pfree(k.data);
		pfree(v.data);
	}
	return found;
}

/*
 * What a fragment's commits make: each one's nodes that its first parent
 * lacks (commit_created()), its first parent read here, or past the
 * horizon its delta kept when the parent was severed (a commit with
 * neither: all it reaches).  Every chunk the fragment holds that history
 * before it doesn't, born in it: O(what it changed), not O(the database),
 * but for the first fragment, whose oldest commit makes its whole tree.
 * The digest is a hash over their hashes, sorted.  (Chunks are found by
 * the commit that made them, so a fragment's are those born in it, by the
 * generation they were born in.)
 */
static void
fragment_closure(List *commits, ProllyHash *digest, int64 *chunks,
				 int64 *bytes, List **chunk_list, List **deltas)
{
	Closure		c = {0};
	ListCell   *lc;
	ProllyHashCtx *ctx;
	HTAB	   *in = id_set("prolly fragment commits", sizeof(ProllyHash));

	c.seen = id_set("prolly fragment chunks", sizeof(ProllyHash));
	initStringInfo(&c.buf);
	foreach(lc, commits)
		(void) set_add(in, lfirst(lc));
	if (deltas)
		*deltas = NIL;
	foreach(lc, commits)
	{
		ProllyHash *id = lfirst(lc);
		ProllyCommitInfo info;
		ProllyCommitInfo pinfo;
		bool		parent_here;

		CHECK_FOR_INTERRUPTS();
		if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
		{
			prolly_window_check_missing(id);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("commit %s is missing from the chunk store",
							prolly_hash_hex(id))));
		}
		if (info.nparents == 0)
		{
			commit_created(&c, id, &info, NULL);
			continue;
		}
		/* (its data here: commit objects are kept, a severed one's data not) */
		parent_here = PROLLY_CALL(dc.refs, commit_get, &info.parents[0], &pinfo) &&
			PROLLY_CALL(tree_env.chunk_store, has, tree_env.chunks,
						&pinfo.root.hash);

		/*
		 * (by the fragment, not by what the store holds: a first parent in
		 * it is walked against, here whenever the fragment is; one outside
		 * it, from the delta kept, which a sever keeps, as found then, for
		 * a commit that has none yet, so its repair finds the same)
		 */
		if (set_has(in, &info.parents[0]) && parent_here)
			commit_created(&c, id, &info, &pinfo);
		else if (!kept_delta(&c, id))
		{
			if (deltas)
			{
				Closure		one = {0};
				Delta	   *d = palloc(sizeof(Delta));

				one.seen = id_set("prolly commit delta", sizeof(ProllyHash));
				initStringInfo(&one.buf);
				commit_created(&one, id, &info, parent_here ? &pinfo : NULL);
				foreach_ptr(ProllyHash, h, one.chunks)
					(void) note_node(&c, h);
				d->id = *id;
				d->hashes = one.chunks;
				*deltas = lappend(*deltas, d);
				pfree(one.buf.data);
				hash_destroy(one.seen);
			}
			else
				commit_created(&c, id, &info, parent_here ? &pinfo : NULL);
		}
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
	hash_destroy(in);
}

/* What a sever leaves garbage, found: each node once. */
typedef struct Removed
{
	HTAB	   *seen;
	List	   *hashes;			/* ProllyHash * */
} Removed;

static bool
removed_visit(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Removed    *r = arg;

	if (set_add(r->seen, &node->hash))
		r->hashes = lappend(r->hashes, memcpy(palloc(sizeof(ProllyHash)),
											  &node->hash, sizeof(ProllyHash)));
	return true;
}

/* The nodes parent (pinfo) has that commit info lacks, into r. */
static void
commit_removed(Removed *r, const ProllyCommitInfo *info,
			   const ProllyCommitInfo *pinfo)
{
	PROLLY_CALL(dc.canonicalizer, walk_changes, &info->root, &pinfo->root,
				removed_visit, r);
	if (pinfo->has_dependencies)
		PROLLY_CALL(tree, diff_nodes, &tree_env,
					info->has_dependencies ? &info->dependencies : NULL,
					&pinfo->dependencies, removed_visit, r);
}

/* Commit id's nodes that each of its parents here has and it lacks. */
static void
removed_by(Removed *r, const ProllyHash *id, const ProllyHash *only)
{
	ProllyCommitInfo info;

	if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
		return;
	for (int i = 0; i < info.nparents; i++)
	{
		ProllyCommitInfo pinfo;

		CHECK_FOR_INTERRUPTS();
		/* (a parent severed before: what it lacks was counted then) */
		if ((only != NULL && !same_id(only, &info.parents[i])) ||
			is_severed(&info.parents[i]) ||
			!PROLLY_CALL(dc.refs, commit_get, &info.parents[i], &pinfo) ||
			!PROLLY_CALL(tree_env.chunk_store, has, tree_env.chunks,
						 &pinfo.root.hash))
			continue;
		commit_removed(r, &info, &pinfo);
	}
}

/*
 * What a sever leaves garbage, as the chunk store's estimate wants it:
 * each node of a commit's parent that the commit no longer has, for each
 * commit severed and each boundary commit (against the parent severed).
 * Along a line of history, a node reached from the fragment and not from
 * the boundary was replaced by one of those commits, so it is among them;
 * one a ref or a resurrection keeps is counted too, which only makes a
 * retire read a file it frees less of.  The created sets' diffs the other
 * way: O(what the fragment changed).  A parent past the horizon already
 * was counted when it was severed (as a boundary commit's), so not again.
 */
static List *
fragment_removed(Plan *plan)
{
	Removed		r = {0};

	r.seen = id_set("prolly fragment removed", sizeof(ProllyHash));
	foreach_ptr(PlanCommit, pc, plan->severed)
		removed_by(&r, &pc->id, NULL);
	foreach_ptr(ProllyHash, pair, plan->edges)
		removed_by(&r, &pair[0], &pair[1]);
	hash_destroy(r.seen);
	return r.hashes;
}

/*
 * Every chunk a fragment's commits reach: each one's whole tree, the
 * chunks born before the fragment too.  What a repair of it from an
 * archive needs, where no older fragment's archive is that one.
 */
static List *
fragment_reach(List *commits)
{
	Closure		c = {0};
	ListCell   *lc;

	c.prune = true;
	c.seen = id_set("prolly fragment reach", sizeof(ProllyHash));
	initStringInfo(&c.buf);
	foreach(lc, commits)
	{
		ProllyHash *id = lfirst(lc);
		ProllyCommitInfo info;

		CHECK_FOR_INTERRUPTS();
		if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("commit %s is missing from the chunk store",
							prolly_hash_hex(id))));
		commit_created(&c, id, &info, NULL);
	}
	pfree(c.buf.data);
	hash_destroy(c.seen);
	return c.chunks;
}


/*
 * Whether an older fragment, still severed, was archived anywhere but url
 * (its chunks a fragment reaches, born before it, are there, not at url).
 */
static const char *
archived_elsewhere(const char *url)
{
	foreach_ptr(FragmentRecord, r, fragment_records())
		if (!r->f.deleted && r->f.location != NULL &&
			strcmp(r->f.location, url) != 0)
			return r->f.location;
	return NULL;
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
				 List *chunk_list, HTAB *verified, List *reach,
				 const char *elsewhere)
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

		/*
		 * (what the fragment reaches, but an older fragment's data: that
		 * is archived elsewhere, or was deleted, and may be gone here)
		 */
		if (PROLLY_HAS(dc.transfer, send_kept))
			sent = (int64) PROLLY_CALL(dc.transfer, send_kept, rr, remote,
									   f->tops, f->ntops);
		else
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

		/*
		 * (and, an older fragment archived elsewhere, that this archive has
		 * all the fragment reaches: a repair from it receives that)
		 */
		if (reach != NIL)
		{
			int			nr = list_length(reach);
			ProllyHash *rh = palloc_array(ProllyHash, nr);
			bool	   *has = palloc_array(bool, nr);
			int			lacking = 0;

			i = 0;
			foreach_ptr(ProllyHash, h, reach)
				rh[i++] = *h;
			if (PROLLY_HAS(rr, has_many))
				PROLLY_CALL(rr, has_many, remote, rh, nr, has);
			else
				for (i = 0; i < nr; i++)
					has[i] = PROLLY_CALL(rr, has, remote, &rh[i]);
			for (i = 0; i < nr; i++)
				if (!has[i])
					lacking++;
			if (lacking > 0)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("the archive at %s lacks %d of the %d chunks the history severed reaches",
								url, lacking, nr),
						 errdetail("They were made before it, and archived at %s.  Nothing is severed.",
								   elsewhere),
						 errhint("Repair the history archived there, or archive there.")));
			pfree(rh);
			pfree(has);
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
			   int *nstops, ProllyHash **stops, List **deltas)
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
					 want_chunks ? chunk_list : NULL, deltas);

	/*
	 * its tops: each parent the boundary keeps no more, and each commit
	 * beneath the horizon kept no more, once
	 */
	tops = palloc_array(ProllyHash, Max(list_length(plan->tops), 1));
	foreach(lc, plan->tops)
		tops[ntops++] = *(ProllyHash *) lfirst(lc);
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

/* A size limit's cut by height: this many parts of what is kept, one cut. */
#define SIZE_HEIGHT_PARTS	16

/*
 * The height a size limit cuts below where its dates cut nothing: a
 * sixteenth of the heights the plan keeps, at least one, above the
 * lowest.  Heights only grow along history, so it is a prefix of every
 * path, as the dates' cut is; 0 if nothing but heads is kept.
 */
static int64
height_cut(Plan *plan)
{
	int64		lowest = PG_INT64_MAX;
	int64		highest = 0;

	foreach_ptr(PlanCommit, pc, plan->kept)
	{
		lowest = Min(lowest, pc->e.height);
		highest = Max(highest, pc->e.height);
	}
	if (lowest >= highest)
		return 0;
	return lowest + Max((highest - lowest) / SIZE_HEIGHT_PARTS, 1);
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
	List	   *deltas = NIL;
	List	   *removed = NIL;
	HTAB	   *verified = id_set("prolly archive verified", sizeof(ProllyHash));
	char	   *archive_url = NULL;
	ProllyWindowPolicy policy;

	ProllyHash	planned;
	ProllyHash	held;
	int64		size_height = 0;

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
		make_plan(&plan, before, size_cut, size_height);

		/*
		 * (a size limit whose dates cut nothing, a commit dated old above
		 * history dated new: cut by height instead, a part of what is
		 * kept here, so the limit still holds)
		 */
		if (plan.severed == NIL && size_cut != DT_NOBEGIN && before == NULL &&
			size_height == 0 && (size_height = height_cut(&plan)) > 0)
			make_plan(&plan, before, size_cut, size_height);
		if (plan.severed == NIL)
		{
			/*
			 * (nothing to sever, but the horizon told of new edges, or
			 * commits dated ahead seen first)
			 */
			if (plan.orphans != NIL || plan.arrivals != NIL)
			{
				if (plan.orphans != NIL)
					horizon_lock(ExclusiveLock);
				PROLLY_CALL(dc.refs, lock);
				if (plan.orphans != NIL && record_orphans(&plan))
					horizon_changed();
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
					   &chunk_list, &nstops, &stops, &deltas);
		if (archive_url)
		{
			const char *elsewhere = archived_elsewhere(archive_url);

			result->sent += archive_fragment(archive_url, &f, chunk_list,
											 verified,
											 elsewhere ? fragment_reach(ids) : NIL,
											 elsewhere);
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

	/* (what it leaves garbage, found before anything is held) */
	removed = PROLLY_HAS(tree_env.chunk_store, note_garbage) ?
		fragment_removed(&plan) : NIL;

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

		make_plan(&again, before, size_cut, size_height);
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
	{
		/* (the generation each top is in now: a repair puts it back there) */
		uint32	   *epochs = palloc0_array(uint32, Max(f.ntops, 1));

		last_top_epoch = 0;
		for (int i = 0; i < f.ntops; i++)
		{
			if (PROLLY_HAS(tree_env.chunk_store, epoch_of))
				epochs[i] = PROLLY_CALL(tree_env.chunk_store, epoch_of,
										tree_env.chunks, &f.tops[i]);
			last_top_epoch = Max(last_top_epoch, epochs[i]);
		}
		fragment_encode(&v, &f,
						PROLLY_HAS(tree_env.chunk_store, generation) ?
						PROLLY_CALL(tree_env.chunk_store, generation,
									tree_env.chunks) : 0,
						nstops, stops, epochs);
		pfree(epochs);
	}
	{
		ProllyKey	key = {(const uint8 *) k.data, k.len};
		ProllyValue value = {(const uint8 *) v.data, v.len};

		PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
	}
	/*
	 * (what each boundary commit makes over its first parent, severed
	 * now, kept for when it falls in a fragment itself)
	 */
	(void) record_orphans(&plan);
	prune_pending();
	/* (each commit severed: its data, not its commit object) */
	foreach_ptr(PlanCommit, pc, plan.severed)
	{
		ProllyCommitInfo info;

		if (!PROLLY_CALL(dc.refs, commit_get, &pc->id, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("commit %s is missing from the chunk store",
							prolly_hash_hex(&pc->id))));
		severed_put(&pc->id, &info, f.number);
	}
	/* (the fragment's commits' deltas over parents outside it, as found) */
	foreach_ptr(Delta, d, deltas)
		write_delta(&d->id, d->hashes);
	foreach(lc, plan.edges)
	{
		ProllyHash *pair = lfirst(lc);
		ProllyCommitInfo binfo;
		ProllyCommitInfo pinfo;

		if (PROLLY_CALL(dc.refs, commit_get, &pair[0], &binfo) &&
			binfo.nparents > 0 &&
			binfo.parents[0].len == pair[1].len &&
			memcmp(binfo.parents[0].bytes, pair[1].bytes, pair[1].len) == 0 &&
			PROLLY_CALL(dc.refs, commit_get, &pair[1], &pinfo))
			keep_delta(&pair[0], &binfo, &pinfo);
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

	/*
	 * (the store's estimate of its files' garbage: the files the fragment
	 * leaves most garbage in are the ones a retire chooses)
	 */
	if (removed != NIL)
	{
		ProllyHash *hashes = palloc_array(ProllyHash, list_length(removed));
		int			n = 0;

		foreach_ptr(ProllyHash, h, removed)
			hashes[n++] = *h;
		(void) PROLLY_CALL(tree_env.chunk_store, note_garbage,
						   tree_env.chunks, hashes, n);
		pfree(hashes);
	}
	/* (outside the window no commit keeps its page root) */
	if (PROLLY_HAS(dc.refs, forget_kept))
		foreach(lc, plan.severed)
			PROLLY_CALL(dc.refs, forget_kept, &((PlanCommit *) lfirst(lc))->id);

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
				 errdetail("The boundary is %s.  The fragment's commits made %lld chunks (%lld bytes), those no kept commit reaches freed by the next collection.",
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
#define SIZE_PENDING_SETTING "history_window.size_pending"
#define SIZE_RETRY_SETTING	"history_window.size_retry_at"
#define MEASURED_SETTING	"history_window.measured"
#define OVER_COUNT_SETTING	"history_window.over_count"
#define FULL_WANTED_SETTING	"history_window.full_wanted"
#define RETIRE_WANTED_SETTING "history_window.retire_wanted"
#define RETIRE_FAILURES_SETTING "history_window.retire_failures"
#define RETIRE_RETRY_SETTING "history_window.retire_retry_at"
#define RETIRE_WANTED_AT_SETTING "history_window.retire_wanted_at"

/* Background retires that fail this many times in a row: a full collection. */
#define RETIRE_FAILURES_MOST	3
#define LAST_FULL_SETTING	"history_window.last_full"
#define SEALED_FILE			"prolly_window_sealed"
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
			if (!is_severed(&info.parents[i]) &&
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
 * lineage's policy (its own window, min_commits and batch over the
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
		*any = true;
		batch = p.batch > 0 ? p.batch : DEFAULT_BACKGROUND_BATCH;
		cut = now - p.keep_time;
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

static bool retires(void);


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
	/*
	 * (archiving with nowhere to archive to, under the default window:
	 * history stays here, nothing severed, and the background says nothing
	 * of it; roots past the window are let go of all the same, by root
	 * retention.  A database that sets a window of its own is told.)
	 */
	if (db.action == PROLLY_WINDOW_ARCHIVE && db.archive == NULL &&
		database_setting("prolly.history_window") == NULL)
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

	/*
	 * (what only a fragment reached freed by a retire of the generations
	 * holding it, where the reclamation retires: a full collection's cost
	 * is the whole store's)
	 */
	if (r.fragment > 0 && retires())
	{
		config_int_set(RETIRE_WANTED_SETTING,
					   Max(config_int(RETIRE_WANTED_SETTING, 0),
						   (int64) last_top_epoch + 1), false);
		/* (since when, for how long another's sever may hold it off) */
		if (config_int(RETIRE_WANTED_AT_SETTING, 0) == 0)
			config_int_set(RETIRE_WANTED_AT_SETTING, GetCurrentTimestamp(),
						   false);
	}
	else if (r.fragment > 0)
		config_int_set(FULL_WANTED_SETTING, 1, false);
	if (want_full)
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
 * Is another backend severing (the severs' turn taken)?  A collection
 * begun now would wait for its transaction, an archive's transfer long,
 * and a background sever waited for gives way.
 */
static bool
sever_running(void)
{
	LOCKTAG		tag;
	int			n = 0;

	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, HORIZON_LOCK_CLASSID, 1, 0);
	(void) GetLockConflicts(&tag, ExclusiveLock, &n);
	return n > 0;
}

/*
 * Is a retire held off for another backend's sever?  Only for so long: until
 * prolly.history_collect_interval (an hour where unset) has passed since
 * the retire was wanted (noted with the want, by the sever that made the
 * fragment), after which the retire runs all the same and waits for the
 * sever's transaction, so severs that keep the turn taken don't hold a
 * wanted retire off for good.  Nothing is written here: a write of the
 * refs would wait for the running sever's transaction, which holds them
 * until it commits, an archive's transfer and all.  (A want with no time
 * noted is held off.)
 */
static bool
sever_held_off(void)
{
	int64		since;

	if (!sever_running())
		return false;
	since = config_int(RETIRE_WANTED_AT_SETTING, 0);
	return since == 0 || GetCurrentTimestamp() - since < collect_interval();
}

/* Does the database's reclamation retire generations, as it chooses them? */
static bool
retires(void)
{
	return dc.reclamation != NULL && PROLLY_HAS(dc.reclamation, retire) &&
		PROLLY_HAS(dc.reclamation, choose);
}

/* The newest fragment archived and found whole there: its generation. */
static bool
note_archived_generation(const ProllyKey *key, const ProllyValue *value,
						 void *arg)
{
	FragmentRecord r;

	if (key->len != 9 || key->data[0] != KEY_FRAGMENT)
		return false;
	fragment_decode((const char *) value->data, value->len,
					(int64) get_be64((const char *) key->data + 1), &r);
	if (r.f.deleted || r.f.location == NULL)
		return true;
	*(uint32 *) arg = r.generation;
	return false;
}

/*
 * A retire is due once a fragment was made since the last; and, whether or
 * not, the newest generation one may take: in archive mode, while some
 * lineage has a window, none newer than the store's generation as the
 * newest fragment archived was severed (0: none was), so that what a
 * sever has yet to archive is never in a retire chosen meanwhile.  (Its
 * chunks are the refs' until it is recorded, which happens only once the
 * archive holds them whole; the limit keeps that so by generation too,
 * whatever a retire's marking finds.)
 */
static bool
archive_retire_due(uint32 *max_hi, uint32 *want)
{
	ProllyWindowPolicy db;
	bool		any;
	int64		wanted;

	window_init();
	archive_policy(NULL, &db);
	*max_hi = PG_UINT32_MAX;
	wanted = config_int(RETIRE_WANTED_SETTING, 0);
	*want = wanted > 0 ? (uint32) (wanted - 1) : 0;
	/* (failing: not again until the wait is over, nor once full is wanted) */
	if ((wanted > 0 ? sever_held_off() : sever_running()) ||
		GetCurrentTimestamp() < config_int(RETIRE_RETRY_SETTING, DT_NOBEGIN) ||
		config_int(RETIRE_FAILURES_SETTING, 0) >= RETIRE_FAILURES_MOST)
	{
		*max_hi = 0;
		return wanted > 0;
	}
	(void) moved_cuts(GetCurrentTimestamp(), &any);
	if (db.action == PROLLY_WINDOW_ARCHIVE && any)
	{
		StringInfoData k;
		ProllyKey	from;

		*max_hi = 0;
		initStringInfo(&k);
		number_key(&k, KEY_FRAGMENT, PG_INT64_MAX);
		from.data = (const uint8 *) k.data;
		from.len = k.len;
		PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
					BackwardScanDirection, note_archived_generation, max_hi);
		pfree(k.data);
	}
	return wanted > 0;
}

/*
 * The background's retire done: the fragments it was for no longer
 * waiting, and the store measured as after a full collection, for the
 * size limit (what the retire left is what the limit's next cut weighs).
 */
static void
archive_retired(void)
{
	ProllyWindowPolicy db;
	uint64		bytes;

	window_init();
	archive_policy(NULL, &db);
	bytes = store_bytes();
	config_int_set(RETIRE_WANTED_SETTING, 0, true);
	config_int_set(RETIRE_FAILURES_SETTING, 0, true);
	config_int_set(RETIRE_RETRY_SETTING, 0, true);
	config_int_set(RETIRE_WANTED_AT_SETTING, 0, true);
	config_int_set(MEASURED_SETTING, (int64) bytes, false);
	config_int_set(SIZE_PENDING_SETTING, 0, true);
	if (db.max_bytes > 0 && bytes > (uint64) db.max_bytes)
		config_int_set(OVER_COUNT_SETTING,
					   config_int(OVER_COUNT_SETTING, 0) + 1, false);
	else
		config_int_set(OVER_COUNT_SETTING, 0, true);
}

/*
 * A background retire failed: tried again after a wait that doubles with
 * each failure in a row, from prolly.history_collect_interval (an hour
 * where unset: full collections are no oftener) to a day; after RETIRE_FAILURES_MOST of them, a full
 * collection is wanted instead (collect_due()), which ends the run.
 */
static void
archive_retire_failed(void)
{
	int64		failures;
	int64		wait;

	window_init();
	wait = collect_interval();
	failures = config_int(RETIRE_FAILURES_SETTING, 0) + 1;
	for (int64 i = 1; i < failures && wait < BACKOFF_MOST; i++)
		wait *= 2;
	config_int_set(RETIRE_FAILURES_SETTING, failures, false);
	config_int_set(RETIRE_RETRY_SETTING, GetCurrentTimestamp() + Min(wait, BACKOFF_MOST),
				   false);
	if (failures >= RETIRE_FAILURES_MOST)
		config_int_set(FULL_WANTED_SETTING, 1, false);
}

static void
newest_epoch(uint32 epoch, TimestampTz ended, void *arg)
{
	TimestampTz *newest = arg;

	*newest = Max(*newest, ended);
}

/*
 * The note of the background's last collection (sealed()): a file of the
 * database's directory, beside its store, not in the store, which the
 * note would change, and so have the next boundary's seal collect again,
 * nor in the refs, whose lock the background would then wait for.  Not
 * WAL-logged, nor synced: lost (a crash, a standby promoted, a backup
 * restored), the next seal is due at the next visit, one more collection.
 */
static char *
sealed_path(void)
{
	char	   *dir = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);
	char	   *path = psprintf("%s/%s", dir, SEALED_FILE);

	pfree(dir);
	return path;
}

static TimestampTz
sealed_at(void)
{
	char	   *path = sealed_path();
	char		buf[32];
	int			fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	ssize_t		n = -1;

	if (fd >= 0)
	{
		n = read(fd, buf, sizeof(buf) - 1);
		CloseTransientFile(fd);
	}
	pfree(path);
	if (n <= 0)
		return DT_NOBEGIN;
	buf[n] = '\0';
	return (TimestampTz) strtoll(buf, NULL, 10);
}

/*
 * A batch boundary passed since the store's newest generation ended, or
 * the background's last collection did (sealed()), whichever is later (the
 * database's batch, an hour where it names none): only while some lineage
 * has a window, whose fragments are cut at batches.
 */
static bool
archive_seal_due(void)
{
	ProllyWindowPolicy db;
	TimestampTz now = GetCurrentTimestamp();
	TimestampTz newest = DT_NOBEGIN;
	TimestampTz boundary;
	int64		batch;
	bool		any;

	window_init();
	if (!PROLLY_HAS(tree_env.chunk_store, epochs) || sever_running())
		return false;
	(void) moved_cuts(now, &any);
	if (!any)
		return false;
	archive_policy(NULL, &db);
	batch = db.batch > 0 ? db.batch : DEFAULT_BACKGROUND_BATCH;
	boundary = now - (((now % batch) + batch) % batch);
	PROLLY_CALL(tree_env.chunk_store, epochs, tree_env.chunks, newest_epoch,
				&newest);
	newest = Max(newest, sealed_at());
	return newest < boundary;
}

/*
 * A background collection ran, ending a generation: the next seal is
 * counted from now, whether or not it settled anything (the store records
 * only the generations its settled files hold).
 */
static void
archive_sealed(void)
{
	char	   *path = sealed_path();
	char	   *temp = psprintf("%s.tmp", path);
	char		buf[32];
	int			len = snprintf(buf, sizeof(buf), INT64_FORMAT,
							   (int64) GetCurrentTimestamp());
	int			fd = OpenTransientFile(temp, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
	bool		written;

	/* (a note that can't be written costs a seal more: no error) */
	written = fd >= 0 && write(fd, buf, len) == len;
	if (!written)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", temp)));
	if (fd >= 0)
		CloseTransientFile(fd);
	if (written && rename(temp, path) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						temp, path)));
	pfree(temp);
	pfree(path);
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
	/* (and what retires failed to free) */
	config_int_set(RETIRE_WANTED_SETTING, 0, true);
	config_int_set(RETIRE_FAILURES_SETTING, 0, true);
	config_int_set(RETIRE_RETRY_SETTING, 0, true);
	config_int_set(RETIRE_WANTED_AT_SETTING, 0, true);
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

static bool
collect_delta_key(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	List	  **keys = arg;
	StringInfo	k;

	if (key->len < 1 || key->data[0] != KEY_DELTA)
		return false;
	k = makeStringInfo();
	appendBinaryStringInfo(k, (const char *) key->data, key->len);
	*keys = lappend(*keys, k);
	return true;
}

/* settle()'s question of what a repair's chunks name that isn't here */
typedef struct SettleAbsent
{
	List	   *records;		/* FragmentRecord *: the horizon's */
	FragmentRecord *repairing;
	int			nroots;
	ProllyHash *roots;			/* its commits' data: the roots settled */
	ProllyHash *commits;		/* and the commit naming each */
} SettleAbsent;

/* The commits naming a severed commit's data (its tree or dependencies). */
static int
settle_named_by(void *arg, const ProllyHash *root, ProllyHash **by)
{
	SettleAbsent *sa = arg;
	int			n = 0;

	*by = palloc_array(ProllyHash, Max(sa->nroots, 1));
	for (int i = 0; i < sa->nroots; i++)
	{
		if (same_hash(&sa->roots[i], root))
			(*by)[n++] = sa->commits[i];
	}
	return n;
}

/*
 * A chunk a fragment being repaired names, not here: if it is another
 * fragment's top (an older one, still severed), the generation that one
 * goes back to when repaired, the proposed one if it has none yet (kept in
 * its record, so its repair puts it there).
 */
static uint32
settle_absent(void *arg, const ProllyHash *hash, uint32 proposed)
{
	SettleAbsent *sa = arg;

	foreach_ptr(FragmentRecord, r, sa->records)
	{
		if (r == sa->repairing)
			continue;
		for (int i = 0; i < r->f.ntops; i++)
		{
			if (!same_hash(&r->f.tops[i], hash))
				continue;
			if (r->top_epochs[i] == 0 && proposed > 0)
			{
				r->top_epochs[i] = proposed;
				fragment_rewrite(r);
			}
			return r->top_epochs[i];
		}
	}
	return 0;
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
			/*
			 * (its commits are here: what comes back is their data, each
			 * one's root and dependencies' trees)
			 */
			List	   *commits = fragment_commits(r);
			int			n = list_length(commits);
			ProllyHash *roots = palloc_array(ProllyHash, Max(n, 1));
			ProllyHash *deps = palloc_array(ProllyHash, Max(n, 1));
			int			nroots = 0;
			int			ndeps = 0;

			foreach_ptr(PlanCommit, pc, commits)
			{
				ProllyCommitInfo info;

				if (!PROLLY_CALL(dc.refs, commit_get, &pc->id, &info))
					continue;
				roots[nroots++] = info.root.hash;
				if (info.has_dependencies)
					deps[ndeps++] = info.dependencies.hash;
			}
			(void) PROLLY_CALL(dc.transfer, receive_trees, rr, remote,
							   roots, nroots, deps, ndeps);
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
		fragment_closure(ids, &digest, &chunks, &bytes, NULL, NULL);
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

		/* (its commits' data back) */
		{
			int			max = 2 * Max(list_length(commits), 1);
			ProllyHash *data_roots = palloc_array(ProllyHash, max);
			ProllyHash *namers = palloc_array(ProllyHash, max);
			int			ndata = 0;

			foreach_ptr(PlanCommit, pc, commits)
			{
				ProllyCommitInfo info;

				if (!PROLLY_CALL(dc.refs, commit_get, &pc->id, &info))
					continue;
				severed_drop(&pc->id, &info);
				data_roots[ndata] = info.root.hash;
				namers[ndata++] = pc->id;
				if (info.has_dependencies)
				{
					data_roots[ndata] = info.dependencies.hash;
					namers[ndata++] = pc->id;
				}
			}

			/*
			 * Verified, settled: the commits, kept in settled files, name
			 * the data received, and a default collection doesn't read
			 * settled files.  Each tree goes back to the generation its
			 * commit's file lists it by.
			 */
			if (PROLLY_HAS(tree_env.chunk_store, settle))
			{
				SettleAbsent sa = {records, r, ndata, data_roots, namers};

				PROLLY_CALL(tree_env.chunk_store, settle, tree_env.chunks,
							data_roots, NULL, ndata,
							settle_absent, settle_named_by, &sa);
			}
			pfree(data_roots);
			pfree(namers);
		}


		/*
		 * (its edges and record let go of: the boundary keeps its parents;
		 * and a boundary's delta over a first parent back here, which a
		 * walk finds again)
		 */
		PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
					ForwardScanDirection, collect_edge_of, &eo);
		foreach_ptr(StringInfoData, ek, eo.keys)
		{
			ProllyKey	key = {(const uint8 *) ek->data, ek->len};
			ProllyHash	parent;
			ProllyHash	boundary;
			ProllyCommitInfo binfo;
			const char *p = ek->data + 1;
			const char *end = ek->data + ek->len;

			p = get_id(p, end, &parent);
			(void) get_id(p, end, &boundary);
			PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, NULL);
			if (PROLLY_CALL(dc.refs, commit_get, &boundary, &binfo) &&
				binfo.nparents > 0 && same_hash(&binfo.parents[0], &parent))
				drop_delta(&boundary);
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
	/* (the horizon empty, no boundary's delta is wanted any more) */
	if (repaired > 0 && fragment_records() == NIL)
	{
		char		d = KEY_DELTA;
		ProllyKey	from = {(const uint8 *) &d, 1};
		List	   *keys = NIL;

		PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
					ForwardScanDirection, collect_delta_key, &keys);
		foreach_ptr(StringInfoData, dk, keys)
		{
			ProllyKey	key = {(const uint8 *) dk->data, dk->len};

			PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, NULL);
		}
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

bool
prolly_window_cut_edge(void *arg, const ProllyHash *source,
					   const ProllyHash *target)
{
	ProllyDatabaseComponents c;
	ProllyCommitInfo info;

	prolly_database_components(&c);
	if (!prolly_window_archived(source) ||
		!PROLLY_CALL(c.refs, commit_get, source, &info))
		return false;
	return same_hash(&info.root.hash, target) ||
		(info.has_dependencies && same_hash(&info.dependencies.hash, target));
}

static void
note_fragment(const ProllyWindowFragment *f, void *arg)
{
	*(bool *) arg = true;
}

bool
prolly_window_severed(void)
{
	ProllyDatabaseComponents c;
	bool		any = false;

	prolly_database_components(&c);
	if (c.history_window == NULL || !PROLLY_HAS(c.history_window, fragments))
		return false;
	PROLLY_CALL(c.history_window, fragments, note_fragment, &any);
	return any;
}

void
prolly_window_check_missing(const ProllyHash *id)
{
	if (prolly_window_archived(id))
	{
		pgstat_count_prolly(PGSTAT_PROLLY_SEVERED_REFUSALS, 1);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("commit %s is past the history horizon",
						prolly_hash_hex(id)),
				 errdetail("History before it was severed from this database: archived, or deleted."),
				 errhint("Run dolt_history_repair() to reattach it.")));
	}
}

/* Does the horizon hold any fragment? */
static bool
any_fragment(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	if (key->len == 9 && key->data[0] == KEY_FRAGMENT)
		*(bool *) arg = true;
	return false;				/* (the first is enough) */
}

static bool
horizon_any(void)
{
	StringInfoData k;
	ProllyKey	from;
	bool		any = false;

	initStringInfo(&k);
	number_key(&k, KEY_FRAGMENT, PG_INT64_MAX);
	from.data = (const uint8 *) k.data;
	from.len = k.len;
	pgstat_count_prolly(PGSTAT_PROLLY_HORIZON_ENTRIES_READ, 1);
	PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
				BackwardScanDirection, any_fragment, &any);
	pfree(k.data);
	return any;
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
	{
		pgstat_count_prolly(PGSTAT_PROLLY_SEVERED_REFUSALS, 1);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("commit %s isn't in this database: it may be past the history horizon",
						prolly_hash_hex(id)),
				 errdetail("History before the horizon was severed from this database: archived, or deleted."),
				 errhint("Run dolt_history_repair() to reattach it.")));
	}
}

typedef struct RootOf
{
	StringInfoData prefix;		/* 'r' and the tree */
	bool		found;
	ProllyHash	commit;
} RootOf;

static bool
root_of(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	RootOf	   *ro = arg;

	if (key->len <= (Size) ro->prefix.len ||
		memcmp(key->data, ro->prefix.data, ro->prefix.len) != 0)
		return false;
	(void) get_id((const char *) key->data + ro->prefix.len,
				  (const char *) key->data + key->len, &ro->commit);
	ro->found = true;
	return false;				/* (one commit severed with it is enough) */
}

void
prolly_window_check_root(const ProllyHash *tree_id)
{
	ProllyDatabaseComponents c;
	ProllyKey	key;
	RootOf		ro;

	if (!IsTransactionState() || tree_id->len == 0)
		return;
	prolly_database_components(&c);
	if (c.history_window != &prolly_window_archive)
		return;
	window_init();
	if (!PROLLY_HAS(dc.refs, side_scan))
		return;
	initStringInfo(&ro.prefix);
	ro.found = false;
	appendStringInfoChar(&ro.prefix, KEY_ROOT);
	put_id(&ro.prefix, tree_id);
	key.data = (const uint8 *) ro.prefix.data;
	key.len = ro.prefix.len;
	PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &key, ForwardScanDirection,
				root_of, &ro);
	pfree(ro.prefix.data);
	if (ro.found)
	{
		pgstat_count_prolly(PGSTAT_PROLLY_SEVERED_REFUSALS, 1);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("commit %s is past the history horizon",
						prolly_hash_hex(&ro.commit)),
				 errdetail("Its data was severed from this database: archived, or deleted.  Its history is kept."),
				 errhint("Run dolt_history_repair() to reattach it.")));
	}
}

bool
prolly_window_rows_severed(const ProllyHash *id)
{
	static LocalTransactionId told = InvalidLocalTransactionId;

	if (!prolly_window_archived(id))
		return false;
	pgstat_count_prolly(PGSTAT_PROLLY_SEVERED_REFUSALS, 1);
	if (told != MyProc->vxid.lxid)
	{
		told = MyProc->vxid.lxid;
		ereport(NOTICE,
				(errmsg("commits past the history horizon have no rows here"),
				 errdetail("The data of commit %s, and of the history before it, was severed from this database; the commits are listed still.",
						   prolly_hash_hex(id)),
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
	.window = archive_window,
	.in_window = archive_in_window,
	.retire_due = archive_retire_due,
	.retired = archive_retired,
	.retire_failed = archive_retire_failed,
	.seal_due = archive_seal_due,
	.sealed = archive_sealed,
};
