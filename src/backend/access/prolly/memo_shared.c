/*-------------------------------------------------------------------------
 *
 * memo_shared.c
 *	  Value memo implementation "shared": a bounded table in shared memory
 *	  keyed by database, TOAST relation, value ID, the xmin of the value's
 *	  first chunk, and the column's type.
 *
 * A value ID isn't a name for content (prolly_memo.h); with its first
 * chunk's xmin it is.  A value inserted before a branch was made has the
 * same first chunk on both branches, and one inserted after gets its
 * inserting transaction's xmin, which no other branch's value has (a
 * transaction writes on one branch).  So a lookup reads the first chunk
 * by the TOAST relation's index (one probe, against reading and hashing
 * the value), and finds the entry by the pointer and that xmin; and by
 * the column's type, whose encoding made the reference.
 *
 * The xmin is the raw one, which freezing keeps, and which repeats after
 * 2^32 transactions.  Each entry records the transaction ID it was filled
 * at, and one filled 2^31 or more transactions ago is dropped rather than
 * matched.  A value whose first chunk is frozen when it would be
 * remembered isn't: its xmin may be older than that, and another value
 * given its ID and the same raw xmin could then come while the entry
 * lives.  An xmin that isn't frozen is less than 2^31 transactions old,
 * so it can't repeat before its entry is dropped.
 *
 * A transaction's entries are kept in its backend until it commits (they
 * name chunks no one else can read until then), and forgotten if it, or
 * the subtransaction that made them, aborts, or if it is prepared (they
 * wait for no COMMIT PREPARED).  A parallel worker's are forgotten when it
 * ends: its leader may yet abort.  At most as many as the shared table
 * holds are kept, one per
 * key.  A hit is also checked against the chunk store, in case anything
 * else removed the tree; an entry of the transaction's that fails it is
 * dropped.
 *
 * The table has prolly_value_memo_entries slots (a server setting: values
 * stored by content need no memo, only those still in TOAST relations)
 * under a spinlock; a key is looked for
 * in MEMO_PROBE slots from its hash, and a new entry takes the first free
 * or matching one there, else the oldest.  Nothing outlives a restart.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/memo_shared.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_shmem.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/toast_internals.h"
#include "access/transam.h"
#include "access/xact.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/spin.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "varatt.h"

#define MEMO_PROBE		8
#define MEMO_REF_MAX	(1 + PROLLY_HASH_MAX_LEN + 10)
#define MEMO_MAX_AGE	(UINT64CONST(1) << 31)

typedef struct MemoKey
{
	Oid			dboid;
	Oid			toastrelid;
	uint64		valueid;
	TransactionId xmin;
	Oid			typid;
} MemoKey;

typedef struct MemoEntry
{
	MemoKey		key;
	uint64		filled;			/* full transaction ID; 0 if free */
	uint8		reflen;
	uint8		ref[MEMO_REF_MAX];
} MemoEntry;

typedef struct MemoShared
{
	slock_t		mutex;
	int			nentries;
	MemoEntry	entries[FLEXIBLE_ARRAY_MEMBER];
} MemoShared;

int			prolly_value_memo_entries = 8192;

/* An entry of this backend's transaction, not yet everyone's. */
typedef struct Pending
{
	MemoKey		key;			/* the hash key */
	SubTransactionId subxid;	/* the subtransaction that made it */
	uint8		reflen;
	uint8		ref[MEMO_REF_MAX];
} Pending;

static MemoShared *memo = NULL;
static HTAB *pending = NULL;	/* by key; NULL if none */
static SubTransactionId pending_newest; /* no subxid of theirs is later */
static bool callbacks_registered = false;
static ProllyMemoCounters counts;

/*
 * The pointer the last lookup keyed, for the remember() that follows it
 * (a miss's): taken once, since the value may be gone by the next.
 */
static char last_pointer[TOAST_OID8_POINTER_SIZE];
static Size last_pointer_len = 0;
static MemoKey last_key;
static bool last_frozen;

static MemoShared *
shared(void)
{
	if (memo == NULL)
		memo = prolly_shmem_area(&prolly_memo_shared.info);
	if (memo == NULL)
		elog(ERROR, "prolly value memo has no shared memory");
	return memo;
}

static Size
shared_shmem_size(void)
{
	return add_size(offsetof(MemoShared, entries),
					mul_size(prolly_value_memo_entries, sizeof(MemoEntry)));
}

static void
shared_shmem_init(void *area)
{
	MemoShared *m = area;

	SpinLockInit(&m->mutex);
	m->nentries = prolly_value_memo_entries;
	memset(m->entries, 0, sizeof(MemoEntry) * m->nentries);
}

/*
 * The key of the value pointer names now: false if its first chunk isn't
 * there.  *frozen says whether that chunk's xmin is frozen.  With cached,
 * the last lookup's key if it was of the same pointer.
 */
static bool
key_of(const void *pointer, Oid typid, MemoKey *key, bool *frozen, bool cached)
{
	toast_external_data ext;
	Relation	toastrel;
	Relation   *toastidxs;
	int			nidx;
	int			valid;
	ScanKeyData skey[2];
	SysScanDesc scan;
	HeapTuple	tup;
	bool		found = false;
	Size		len = VARSIZE_ANY(pointer);

	if (cached && len == last_pointer_len &&
		memcmp(pointer, last_pointer, len) == 0 && last_key.typid == typid)
	{
		*key = last_key;
		*frozen = last_frozen;
		last_pointer_len = 0;
		return true;
	}
	last_pointer_len = 0;

	toast_external_info_get(pointer, &ext);
	memset(key, 0, sizeof(*key));
	key->dboid = MyDatabaseId;
	key->toastrelid = ext.toastrelid;
	key->valueid = ext.valueid;
	key->typid = typid;

	toastrel = table_open(ext.toastrelid, AccessShareLock);
	valid = toast_open_indexes(toastrel, AccessShareLock, &toastidxs, &nidx);
	toast_valueid_scankey_init(&skey[0],
							   TupleDescAttr(toastrel->rd_att, 0)->atttypid,
							   ext.valueid);
	ScanKeyInit(&skey[1], (AttrNumber) 2, BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));
	scan = systable_beginscan_ordered(toastrel, toastidxs[valid],
									  get_toast_snapshot(), 2, skey);
	tup = systable_getnext_ordered(scan, ForwardScanDirection);
	if (tup != NULL)
	{
		key->xmin = HeapTupleHeaderGetRawXmin(tup->t_data);
		*frozen = HeapTupleHeaderXminFrozen(tup->t_data);
		found = true;
	}
	systable_endscan_ordered(scan);
	toast_close_indexes(toastidxs, nidx, AccessShareLock);
	table_close(toastrel, AccessShareLock);

	if (found && len <= sizeof(last_pointer))
	{
		memcpy(last_pointer, pointer, len);
		last_pointer_len = len;
		last_key = *key;
		last_frozen = *frozen;
	}
	return found;
}

static int
first_slot(const MemoKey *key)
{
	return (int) (hash_bytes((const unsigned char *) key, sizeof(*key)) %
				  shared()->nentries);
}

static bool
same_key(const MemoKey *a, const MemoKey *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static bool
shared_lookup(const ProllyValueStore *values, const void *pointer, Oid typid,
			  StringInfo ref)
{
	MemoKey		key;
	bool		frozen;
	uint8		buf[MEMO_REF_MAX];
	int			len = -1;
	bool		own = false;

	if (!VARATT_IS_EXTERNAL_ONDISK(pointer) ||
		!key_of(pointer, typid, &key, &frozen, false))
	{
		counts.misses++;
		pgstat_count_prolly(PGSTAT_PROLLY_MEMO_MISSES, 1);
		return false;
	}

	/* This transaction's own, then everyone's. */
	if (pending != NULL)
	{
		Pending    *p = hash_search(pending, &key, HASH_FIND, NULL);

		if (p != NULL)
		{
			len = p->reflen;
			memcpy(buf, p->ref, len);
			own = true;
		}
	}
	if (len < 0)
	{
		MemoShared *m = shared();
		uint64		now = U64FromFullTransactionId(ReadNextFullTransactionId());
		int			slot = first_slot(&key);

		SpinLockAcquire(&m->mutex);
		for (int i = 0; i < MEMO_PROBE; i++)
		{
			MemoEntry  *e = &m->entries[(slot + i) % m->nentries];

			if (e->filled == 0 || !same_key(&e->key, &key))
				continue;
			if (now - e->filled >= MEMO_MAX_AGE)
				e->filled = 0;	/* too old to tell its xmin from a new one */
			else
			{
				len = e->reflen;
				memcpy(buf, e->ref, len);
			}
			break;
		}
		SpinLockRelease(&m->mutex);
	}
	if (len >= 0)
	{
		ProllyValue r = {buf, (uint32) len};

		if (PROLLY_CALL(values->tree, value_present, values->env, &r))
		{
			appendBinaryStringInfo(ref, (const char *) buf, len);
			counts.hits++;
			pgstat_count_prolly(PGSTAT_PROLLY_MEMO_HITS, 1);
			return true;
		}
		if (own)
			(void) hash_search(pending, &key, HASH_REMOVE, NULL);
	}
	counts.misses++;
	pgstat_count_prolly(PGSTAT_PROLLY_MEMO_MISSES, 1);
	return false;
}

static void
publish(void)
{
	MemoShared *m = shared();
	uint64		now = U64FromFullTransactionId(ReadNextFullTransactionId());
	HASH_SEQ_STATUS seq;
	Pending    *p;

	hash_seq_init(&seq, pending);
	while ((p = hash_seq_search(&seq)) != NULL)
	{
		int			slot = first_slot(&p->key);
		MemoEntry  *take = NULL;

		SpinLockAcquire(&m->mutex);
		for (int i = 0; i < MEMO_PROBE; i++)
		{
			MemoEntry  *e = &m->entries[(slot + i) % m->nentries];

			if (e->filled == 0 || same_key(&e->key, &p->key))
			{
				take = e;
				break;
			}
			if (take == NULL || e->filled < take->filled)
				take = e;
		}
		take->key = p->key;
		take->filled = now;
		take->reflen = p->reflen;
		memcpy(take->ref, p->ref, p->reflen);
		SpinLockRelease(&m->mutex);
	}
}

static void
forget_all(void)
{
	if (pending != NULL)
		hash_destroy(pending);
	pending = NULL;
	last_pointer_len = 0;
}

static void
memo_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
			if (pending != NULL)
				publish();
			forget_all();
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			/*
			 * (a worker's leader may still abort; a prepared transaction's
			 * chunks wait for COMMIT PREPARED)
			 */
			forget_all();
			break;
		default:
			break;
	}
}

/*
 * A subtransaction's entries are its parent's when it commits, and they
 * keep its ID: every subtransaction begun since an open one began is that
 * one's descendant, so when a subtransaction aborts, the entries to forget
 * are those of its ID or a later one.
 */
static void
memo_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					  SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS seq;
	Pending    *p;

	if (event != SUBXACT_EVENT_ABORT_SUB)
		return;
	last_pointer_len = 0;
	if (pending == NULL || pending_newest < mySubid)
		return;
	hash_seq_init(&seq, pending);
	while ((p = hash_seq_search(&seq)) != NULL)
		if (p->subxid >= mySubid)
			(void) hash_search(pending, &p->key, HASH_REMOVE, NULL);
	pending_newest = mySubid - 1;	/* (at most) */
}

static void
shared_remember(const void *pointer, Oid typid, const ProllyValue *ref)
{
	MemoKey		key;
	bool		frozen;
	Pending    *p;

	if (!VARATT_IS_EXTERNAL_ONDISK(pointer) || ref->len > MEMO_REF_MAX ||
		!key_of(pointer, typid, &key, &frozen, true) || frozen)
		return;
	if (!callbacks_registered)
	{
		RegisterXactCallback(memo_xact_callback, NULL);
		RegisterSubXactCallback(memo_subxact_callback, NULL);
		callbacks_registered = true;
	}
	if (pending == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(MemoKey);
		ctl.entrysize = sizeof(Pending);
		ctl.hcxt = TopMemoryContext;
		pending = hash_create("prolly value memo", 256, &ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		pending_newest = InvalidSubTransactionId;
	}
	/* (no more than the shared table holds: the rest would be dropped) */
	p = hash_search(pending, &key,
					hash_get_num_entries(pending) < shared()->nentries ?
					HASH_ENTER : HASH_FIND, NULL);
	if (p == NULL)
		return;
	p->subxid = GetCurrentSubTransactionId();
	pending_newest = Max(pending_newest, p->subxid);
	p->reflen = (uint8) ref->len;
	memcpy(p->ref, ref->data, ref->len);
}

static void
shared_counters(ProllyMemoCounters *out, bool reset)
{
	*out = counts;
	if (reset)
		memset(&counts, 0, sizeof(counts));
}

const ProllyValueMemoRoutine prolly_memo_shared = {
	.info = {
		.iface = PROLLY_IFACE_VALUE_MEMO,
		.impl_id = 1,
		.format_version = 1,
		.name = "shared",
		.description = "values TOAST stored out of line, by pointer and their "
		"first chunk's xmin, in a bounded table in shared memory",
	},
	.shmem_size = shared_shmem_size,
	.shmem_init = shared_shmem_init,
	.lookup = shared_lookup,
	.remember = shared_remember,
	.counters = shared_counters,
};
