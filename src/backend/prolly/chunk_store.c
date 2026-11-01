/*-------------------------------------------------------------------------
 *
 * chunk_store.c
 *	  The history store's chunks, from C (prolly/chunk_store.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_store.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "miscadmin.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_format.h"
#include "prolly/chunk_pin.h"
#include "prolly/chunk_store.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

void
ChunkHash(const void *data, size_t len, uint8 *out)
{
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
	uint8		digest[PG_SHA256_DIGEST_LENGTH];

	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, data, len) < 0 ||
		pg_cryptohash_final(ctx, digest, sizeof(digest)) < 0)
		elog(ERROR, "could not hash a chunk: %s", pg_cryptohash_error(ctx));
	pg_cryptohash_free(ctx);
	memcpy(out, digest, CHUNK_HASH_LEN);
}

/*
 * Bytes that aren't hash's are an error: of the chunk read from log at loc,
 * or (log NULL) of a put.
 */
static void
check_hash(const uint8 *hash, const void *data, size_t len, Relation log,
		   uint64 loc)
{
	uint8		actual[CHUNK_HASH_LEN];

	ChunkHash(data, len, actual);
	if (memcmp(actual, hash, CHUNK_HASH_LEN) != 0)
	{
		char		want[2 * CHUNK_HASH_LEN + 1];
		char		got[2 * CHUNK_HASH_LEN + 1];

		hex_encode((const char *) hash, CHUNK_HASH_LEN, want);
		want[2 * CHUNK_HASH_LEN] = '\0';
		hex_encode((const char *) actual, CHUNK_HASH_LEN, got);
		got[2 * CHUNK_HASH_LEN] = '\0';
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 log ?
				 errmsg("stored chunk %s does not match its hash", want) :
				 errmsg("bytes put as chunk %s do not hash to it", want),
				 log ?
				 errdetail("Its %zu bytes at %llu in chunk log \"%s\" hash to %s.",
						   len, (unsigned long long) loc,
						   RelationGetRelationName(log), got) :
				 errdetail("Its %zu bytes hash to %s.", len, got)));
	}
}

bool
ChunkStoreHas(Relation log, Relation idx, const uint8 *hash)
{
	ChunkIndexEntry e;

	return ChunkIndex->lookup(idx, hash, &e);
}

void *
ChunkStoreGet(Relation log, Relation idx, const uint8 *hash, uint32 *len)
{
	ChunkIndexEntry e;
	void	   *data;

	/*
	 * (a chunk freed is taken out, and its bytes reused, once no snapshot
	 * might have found it before: a reader without one has none of that
	 * protection, as a TOAST fetch would not)
	 */
	if (!HaveRegisteredOrActiveSnapshot())
		elog(ERROR, "cannot read the history store without an active snapshot");
	if (!ChunkIndex->lookup(idx, hash, &e))
		return NULL;
	data = palloc_extended(Max(e.len, 1), MCXT_ALLOC_HUGE);
	ChunkLog->read(log, e.loc, data, e.len);
	check_hash(hash, data, e.len, log, e.loc);
	*len = e.len;
	return data;
}

typedef struct ChunkPlace
{
	uint64		loc;
	uint32		len;
	int			at;				/* where it was asked for */
} ChunkPlace;

static int
place_cmp(const void *a, const void *b)
{
	const ChunkPlace *pa = a,
			   *pb = b;

	if (pa->loc != pb->loc)
		return pa->loc < pb->loc ? -1 : 1;
	return pa->at - pb->at;
}

void
ChunkStoreGetMany(Relation log, Relation idx, const uint8 *hashes, int n,
				  void **out, uint32 *lens)
{
	ChunkPlace *places = palloc(Max(n, 1) * sizeof(ChunkPlace));
	int			found = 0;

	if (!HaveRegisteredOrActiveSnapshot())
		elog(ERROR, "cannot read the history store without an active snapshot");
	for (int i = 0; i < n; i++)
	{
		ChunkIndexEntry e;

		out[i] = NULL;
		lens[i] = 0;
		if (ChunkIndex->lookup(idx, hashes + i * CHUNK_HASH_LEN, &e))
		{
			places[found].loc = e.loc;
			places[found].len = e.len;
			places[found].at = i;
			found++;
		}
	}

	/* (in the log's order: its pages each read through once) */
	qsort(places, found, sizeof(ChunkPlace), place_cmp);
	for (int k = 0; k < found; k++)
	{
		ChunkPlace *pl = &places[k];
		void	   *data = palloc_extended(Max(pl->len, 1), MCXT_ALLOC_HUGE);

		CHECK_FOR_INTERRUPTS();
		ChunkLog->read(log, pl->loc, data, pl->len);
		check_hash(hashes + pl->at * CHUNK_HASH_LEN, data, pl->len, log, pl->loc);
		out[pl->at] = data;
		lens[pl->at] = pl->len;
	}
	pfree(places);
}

/* ------------------------------------------------------- references */

static void
leaf_refs(const void *data, size_t len, ChunkRefFn fn, void *arg)
{
}

const ChunkFormatRoutine LeafChunkFormat = {
	.refs = leaf_refs,
};

const ChunkFormatRoutine *ChunkFormat = &LeafChunkFormat;

typedef struct RefList
{
	uint8	   *hashes;
	int			n;
	int			cap;
} RefList;

static void
add_ref(void *arg, const uint8 *hash)
{
	RefList    *list = arg;

	if (list->n == list->cap)
	{
		list->cap = list->cap ? list->cap * 2 : 16;
		list->hashes = list->hashes ?
			repalloc(list->hashes, list->cap * CHUNK_HASH_LEN) :
			palloc(list->cap * CHUNK_HASH_LEN);
	}
	memcpy(list->hashes + list->n * CHUNK_HASH_LEN, hash, CHUNK_HASH_LEN);
	list->n++;
}

static char *
hash_text(const uint8 *hash)
{
	char	   *out = palloc(2 * CHUNK_HASH_LEN + 1);

	hex_encode((const char *) hash, CHUNK_HASH_LEN, out);
	out[2 * CHUNK_HASH_LEN] = '\0';
	return out;
}

static bool put_hashed(Relation log, Relation idx, const uint8 *hash,
					   const void *data, uint32 len);

static void
missing_ref(const uint8 *chunk, const uint8 *target)
{
	ereport(ERROR,
			(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
			 errmsg("chunk %s references chunk %s, which the store does not have",
					hash_text(chunk), hash_text(target))));
}

typedef struct PackChunk
{
	uint8		hash[CHUNK_HASH_LEN];	/* the key */
	int			at;				/* its place in the pack */
} PackChunk;

/* A reference of target's (from chunk) counted, or taken back. */
static void
count_ref(Relation idx, const uint8 *chunk, const uint8 *target, int32 drefs)
{
	ChunkIndexEntry e;

	if (!ChunkIndex->adjust(idx, target, drefs, 0, &e))
		missing_ref(chunk, target);
}

/*
 * Each of a chunk's references the store's, looked up before any is
 * counted: a put refused for one it lacks counts none (a count, then the
 * refusal, would leave a change behind, aborted, for VACUUM)
 */
static void
check_refs(Relation idx, const uint8 *chunk, const RefList *refs, HTAB *pack,
		   const bool *planned)
{
	for (int i = 0; i < refs->n; i++)
	{
		const uint8 *rh = refs->hashes + i * CHUNK_HASH_LEN;
		ChunkIndexEntry e;

		if (pack != NULL)
		{
			PackChunk  *pc = hash_search(pack, rh, HASH_FIND, NULL);

			if (pc != NULL && planned[pc->at])
				continue;
		}
		if (!ChunkIndex->lookup(idx, rh, &e))
			missing_ref(chunk, rh);
	}
}

/*
 * The log and its index bound to each other (an index is one log
 * storage's, a log one index's), each checked before either is: a
 * binding written, then refused, would stay
 */
static void
bind_store(Relation log, Relation idx)
{
	uint64		stamp = ChunkLog->stamp(log);
	uint64		bound_stamp;
	Oid			bound = ChunkIndex->log_of(idx, &bound_stamp);

	/* (bound to another: bind_log says so, writing nothing) */
	if (OidIsValid(bound) &&
		(bound != RelationGetRelid(log) || bound_stamp != stamp))
		ChunkIndex->bind_log(idx, RelationGetRelid(log), stamp);
	ChunkLog->bind_index(log, RelationGetRelid(idx));
	ChunkIndex->bind_log(idx, RelationGetRelid(log), stamp);
}

/*
 * Whether the store has the chunk, held by this transaction if so until
 * it ends: by a change of its counts adding nothing (running, it keeps
 * another's freer off; one freeing the chunk as it is made is waited for,
 * and the chunk, freed, is not found), and pinned, for this transaction's
 * own freeing to pass by.  One this transaction put is held by its entry.
 */
static bool
found_held(Relation idx, const uint8 *hash)
{
	ChunkIndexEntry e;

	if (!ChunkIndex->lookup(idx, hash, &e))
		return false;
	if (ChunkEntryIsMine(&e))
		return true;
	if (!ChunkIndex->adjust(idx, hash, 0, 0, &e))
		return false;
	ChunkPin(hash);
	return true;
}

bool
ChunkStoreHold(Relation idx, const uint8 *hash)
{
	return found_held(idx, hash);
}

/* An entry's place: its bytes reserved in the run prepared (does not fail). */
typedef struct PlaceArg
{
	Relation	log;
	ChunkLogRun *run;
} PlaceArg;

static void
reserve_place(void *arg, ChunkIndexEntry *entry)
{
	PlaceArg   *place = arg;

	entry->loc = entry->len > 0 ?
		ChunkLog->reserve(place->log, place->run, entry->len) : 0;
	entry->log_gen = place->run->generation;
}

bool
ChunkStorePut(Relation log, Relation idx, const uint8 *hash,
			  const void *data, uint32 len)
{
	PreventCommandIfReadOnly("put a chunk");
	check_hash(hash, data, len, NULL, 0);
	return put_hashed(log, idx, hash, data, len);
}

bool
ChunkStorePutData(Relation log, Relation idx, const void *data, uint32 len,
				  uint8 *hash)
{
	PreventCommandIfReadOnly("put a chunk");
	ChunkHash(data, len, hash);
	return put_hashed(log, idx, hash, data, len);
}

/* ChunkStorePut of bytes known to be the chunk hash names */
static bool
put_hashed(Relation log, Relation idx, const uint8 *hash, const void *data,
		   uint32 len)
{
	ChunkIndexEntry e;
	RefList		refs = {0};
	ChunkLogRun run;
	PlaceArg	place;

	/*
	 * (a transaction id first, so that the commit of a put that finds the
	 * chunk there already flushes the WAL that put it, whoever wrote it)
	 */
	(void) GetCurrentTransactionId();

	/* found there already: held until the transaction ends */
	if (found_held(idx, hash))
		return false;

	/*
	 * A store holds a chunk only if it holds every chunk it references: each
	 * edge a reference of the chunk it names, counted now, in this
	 * transaction (its change, running, holds the chunk: no freer frees it
	 * meanwhile, and no pin is needed), taken back if another puts the
	 * chunk first; an abort, or a crash, takes them back with the chunk
	 */
	ChunkFormat->refs(data, len, add_ref, &refs);
	check_refs(idx, hash, &refs, NULL, NULL);
	for (int i = 0; i < refs.n; i++)
		count_ref(idx, hash, refs.hashes + i * CHUNK_HASH_LEN, 1);

	/* (an index is one log's: VACUUM counts the bytes it takes out there) */
	bind_store(log, idx);

	/*
	 * The entry first, its bytes' place reserved as it is put, the bytes
	 * written after: every wait and failure is before the place is taken,
	 * and bytes reserved are the entry's, which (aborted) counts them dead
	 */
	ChunkLog->prepare(log, len, &run);
	place.log = log;
	place.run = &run;
	for (;;)
	{
		memset(&e, 0, sizeof(e));
		memcpy(e.hash, hash, CHUNK_HASH_LEN);
		e.len = len;
		if (ChunkIndex->insert(idx, &e, reserve_place, &place))
			break;

		/* (another put it meanwhile: nothing reserved; gone again, put it) */
		if (found_held(idx, hash))
		{
			ChunkLog->release(log, &run);
			for (int i = 0; i < refs.n; i++)
				count_ref(idx, hash, refs.hashes + i * CHUNK_HASH_LEN, -1);
			return false;
		}
	}
	INJECTION_POINT("chunk-put-entered", NULL);
	if (len > 0)
		ChunkLog->write(log, e.loc, data, len);
	ChunkLog->release(log, &run);

	if (refs.hashes)
		pfree(refs.hashes);
	return true;
}

typedef struct PackOrder
{
	const uint8 *hash;
	int			at;
} PackOrder;

static int
pack_order_cmp(const void *a, const void *b)
{
	return memcmp(((const PackOrder *) a)->hash, ((const PackOrder *) b)->hash,
				  CHUNK_HASH_LEN);
}

/* a pack's bytes reserved for its chunks order[from..to), written */
static void
write_pack(Relation log, const PackOrder *order, int from, int to,
		   const bool *fresh, const uint64 *locs, const void *const *datas,
		   const uint32 *lens)
{
	for (int k = from; k < to; k++)
	{
		int			i = order[k].at;

		CHECK_FOR_INTERRUPTS();
		if (fresh[i] && lens[i] > 0)
			ChunkLog->write(log, locs[i], datas[i], lens[i]);
	}
}

uint64
ChunkStorePutMany(Relation log, Relation idx, int n, const uint8 *hashes,
				  const void *const *datas, const uint32 *lens)
{
	HASHCTL		ctl;
	HTAB	   *pack;
	bool	   *fresh = palloc0(Max(n, 1) * sizeof(bool));
	bool	   *planned = palloc0(Max(n, 1) * sizeof(bool));	/* fresh when looked
																 * up */
	RefList    *refs = palloc0(Max(n, 1) * sizeof(RefList));
	uint64	   *locs = palloc(Max(n, 1) * sizeof(uint64));
	uint64		stored = 0;
	ChunkIndexEntry e;

	PreventCommandIfReadOnly("put a chunk");
	for (int i = 0; i < n; i++)
		check_hash(hashes + i * CHUNK_HASH_LEN, datas[i], lens[i], NULL, 0);
	(void) GetCurrentTransactionId();

	/*
	 * the pack's chunks the store hasn't, each once; those it has held, as a
	 * put's are
	 */
	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = sizeof(PackChunk);
	ctl.hcxt = CurrentMemoryContext;
	pack = hash_create("chunk pack", Max(n, 16), &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < n; i++)
	{
		const uint8 *h = hashes + i * CHUNK_HASH_LEN;
		PackChunk  *pc;
		bool		found;

		CHECK_FOR_INTERRUPTS();
		pc = hash_search(pack, h, HASH_ENTER, &found);
		if (found)
			continue;
		pc->at = i;
		fresh[i] = planned[i] = !found_held(idx, h);
	}

	/*
	 * every chunk a new one references, the store's (its reference counted
	 * now, as a put's are: its change, running, holds it) or the pack's new
	 * ones (counted once they are put: a pack's chunks reference each other
	 * in any order)
	 */
	for (int i = 0; i < n; i++)
		if (fresh[i])
		{
			ChunkFormat->refs(datas[i], lens[i], add_ref, &refs[i]);
			check_refs(idx, hashes + i * CHUNK_HASH_LEN, &refs[i], pack, planned);
		}
	for (int i = 0; i < n; i++)
	{
		if (!fresh[i])
			continue;
		for (int r = 0; r < refs[i].n; r++)
		{
			const uint8 *rh = refs[i].hashes + r * CHUNK_HASH_LEN;
			PackChunk  *pc;

			pc = hash_search(pack, rh, HASH_FIND, NULL);
			if (pc != NULL && planned[pc->at])
				continue;
			count_ref(idx, hashes + i * CHUNK_HASH_LEN, rh, 1);
		}
	}

	bind_store(log, idx);

	/*
	 * Their entries, each one's place reserved as it is put, in their names'
	 * order (packs naming chunks in other orders wait for each other's puts
	 * in one order: no deadlock); then their bytes, written where reserved.
	 * Each entry's waits and failures are before its place is taken, and
	 * bytes reserved are their entry's, which (aborted) counts them dead,
	 * written or not: a chunk another put first reserves none.  Room is made a chunk at a time, so that a
	 * chunk fitting a page freed goes into one; the bytes reserved in a
	 * region are written before it is let go of (it is sealed from where
	 * they end).
	 */
	{
		PackOrder  *order = palloc(Max(n, 1) * sizeof(PackOrder));
		int			nfresh = 0;
		int			unwritten = 0;
		bool		have_run = false;
		ChunkLogRun run;
		PlaceArg	place;

		for (int i = 0; i < n; i++)
			if (fresh[i])
			{
				order[nfresh].hash = hashes + i * CHUNK_HASH_LEN;
				order[nfresh].at = i;
				nfresh++;
			}
		qsort(order, nfresh, sizeof(PackOrder), pack_order_cmp);
		place.log = log;
		place.run = &run;
		for (int k = 0; k < nfresh; k++)
		{
			int			i = order[k].at;

			CHECK_FOR_INTERRUPTS();
			if (!have_run || run.limit - run.next < lens[i])
			{
				write_pack(log, order, unwritten, k, fresh, locs, datas, lens);
				unwritten = k;
				if (have_run)
					ChunkLog->release(log, &run);
				ChunkLog->prepare(log, lens[i], &run);
				have_run = true;
			}
			for (;;)
			{
				memset(&e, 0, sizeof(e));
				memcpy(e.hash, hashes + i * CHUNK_HASH_LEN, CHUNK_HASH_LEN);
				e.len = lens[i];
				if (ChunkIndex->insert(idx, &e, reserve_place, &place))
				{
					locs[i] = e.loc;
					stored++;
					break;
				}

				/* (another put it meanwhile: nothing reserved; gone again, put it) */
				if (found_held(idx, hashes + i * CHUNK_HASH_LEN))
				{
					fresh[i] = false;
					break;
				}
			}
			INJECTION_POINT("chunk-pack-entry", NULL);
		}
		INJECTION_POINT("chunk-put-entered", NULL);
		write_pack(log, order, unwritten, nfresh, fresh, locs, datas, lens);
		if (have_run)
			ChunkLog->release(log, &run);
		pfree(order);
	}

	/*
	 * then each edge to a new chunk of the pack a reference of it; a chunk
	 * another put first takes back those counted of the store's (its own
	 * put counted its references)
	 */
	for (int i = 0; i < n; i++)
	{
		if (!planned[i])
			continue;
		for (int r = 0; r < refs[i].n; r++)
		{
			const uint8 *rh = refs[i].hashes + r * CHUNK_HASH_LEN;
			PackChunk  *pc = hash_search(pack, rh, HASH_FIND, NULL);
			bool		in_pack = pc != NULL && planned[pc->at];

			if (fresh[i] && in_pack)
				count_ref(idx, hashes + i * CHUNK_HASH_LEN, rh, 1);
			else if (!fresh[i] && !in_pack)
				count_ref(idx, hashes + i * CHUNK_HASH_LEN, rh, -1);
		}
		if (refs[i].hashes)
			pfree(refs[i].hashes);
	}
	hash_destroy(pack);
	pfree(fresh);
	pfree(planned);
	pfree(refs);
	pfree(locs);
	return stored;
}

static void
change_root(Relation idx, const uint8 *hash, int32 droots, ChunkIndexEntry *out)
{
	PreventCommandIfReadOnly("change a chunk's roots");
	if (!ChunkIndex->adjust(idx, hash, 0, droots, out))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the store has no chunk %s", hash_text(hash))));
}

void
ChunkStoreRefRoot(Relation idx, const uint8 *hash, ChunkIndexEntry *out)
{
	change_root(idx, hash, 1, out);
}

void
ChunkStoreUnrefRoot(Relation idx, const uint8 *hash, ChunkIndexEntry *out)
{
	change_root(idx, hash, -1, out);
}

/* ------------------------------------------------------- freeing */

uint64
ChunkStoreFree(Relation log, Relation idx, const uint8 *hashes, int n)
{
	RefList		work = {0};
	uint64		freed = 0;

	for (int i = 0; i < n; i++)
		add_ref(&work, hashes + i * CHUNK_HASH_LEN);

	/*
	 * Each chunk nothing holds, freed, and a reference of each chunk it
	 * names let go of, in this transaction: those its references held are
	 * then freed too, and the whole is freed, or not, as it commits
	 */
	for (int i = 0; i < work.n; i++)
	{
		const uint8 *h = work.hashes + i * CHUNK_HASH_LEN;
		ChunkIndexEntry e;
		RefList		kids = {0};
		void	   *data;

		CHECK_FOR_INTERRUPTS();

		/* (pins looked for, then marked, the chunk locked: prolly/chunk_pin.h) */
		if (!ChunkIndex->free(idx, h, ChunkPinned, &e))
			continue;
		data = palloc_extended(Max(e.len, 1), MCXT_ALLOC_HUGE);
		ChunkLog->read(log, e.loc, data, e.len);
		check_hash(h, data, e.len, log, e.loc);
		ChunkFormat->refs(data, e.len, add_ref, &kids);
		pfree(data);
		for (int k = 0; k < kids.n; k++)
		{
			const uint8 *kid = kids.hashes + k * CHUNK_HASH_LEN;
			ChunkIndexEntry ke;

			if (!ChunkIndex->adjust(idx, kid, -1, 0, &ke))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("chunk %s references chunk %s, which the store does not have",
								hash_text(h), hash_text(kid))));
			if (ke.refs == 0 && ke.roots == 0)
				add_ref(&work, kid);
		}
		if (kids.hashes)
			pfree(kids.hashes);
		freed++;
	}
	if (work.hashes)
		pfree(work.hashes);
	return freed;
}

typedef struct UnheldState
{
	RefList		list;
} UnheldState;

static void
collect_unheld(void *arg, const ChunkIndexEntry *e)
{
	if (e->refs == 0 && e->roots == 0)
		add_ref(&((UnheldState *) arg)->list, e->hash);
}

uint64
ChunkStoreFreeUnheld(Relation log, Relation idx)
{
	UnheldState st = {0};
	uint64		freed;

	ChunkIndex->visit(idx, collect_unheld, &st);
	freed = ChunkStoreFree(log, idx, st.list.hashes, st.list.n);
	if (st.list.hashes)
		pfree(st.list.hashes);
	return freed;
}

/* ------------------------------------------------------- the audit */

typedef struct AuditEntry
{
	uint8		hash[CHUNK_HASH_LEN];	/* (the key) */
	uint32		refs;			/* the entry's */
	uint32		roots;
	uint32		len;
	uint64		loc;
	uint64		edges;			/* strong references found to it */
} AuditEntry;

typedef struct AuditState
{
	HTAB	   *chunks;
	uint64		n;
} AuditState;

static void
audit_collect(void *arg, const ChunkIndexEntry *e)
{
	AuditState *as = arg;
	AuditEntry *a;
	bool		found;

	a = hash_search(as->chunks, e->hash, HASH_ENTER, &found);
	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the store has two entries for chunk %s", hash_text(e->hash))));
	a->refs = e->refs;
	a->roots = e->roots;
	a->len = e->len;
	a->loc = e->loc;
	a->edges = 0;
	as->n++;
}

typedef struct EdgeState
{
	AuditState *as;
	const uint8 *from;
	char	   *why;			/* the first fault found, or NULL */
} EdgeState;

static void
audit_edge(void *arg, const uint8 *hash)
{
	EdgeState  *es = arg;
	AuditEntry *a = hash_search(es->as->chunks, hash, HASH_FIND, NULL);

	if (a)
		a->edges++;
	else if (es->why == NULL)
		es->why = psprintf("chunk %s references chunk %s, which the store does not have",
						   hash_text(es->from), hash_text(hash));
}

bool
ChunkStoreCheck(Relation log, Relation idx, uint64 *chunks, uint64 *unheld,
				char **why)
{
	HASHCTL		ctl;
	AuditState	as = {0};
	EdgeState	es = {0};
	HASH_SEQ_STATUS seq;
	AuditEntry *a;

	/* (with no writer meanwhile: a split during it would show chunks twice) */
	LockRelationOid(RelationGetRelid(idx), ShareLock);
	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = sizeof(AuditEntry);
	ctl.hcxt = CurrentMemoryContext;
	as.chunks = hash_create("chunk store audit", 1024, &ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	ChunkIndex->visit(idx, audit_collect, &as);

	/* every chunk's bytes are its own, and every chunk it names is there */
	es.as = &as;
	hash_seq_init(&seq, as.chunks);
	while ((a = hash_seq_search(&seq)) != NULL)
	{
		void	   *data = palloc_extended(Max(a->len, 1), MCXT_ALLOC_HUGE);

		ChunkLog->read(log, a->loc, data, a->len);
		check_hash(a->hash, data, a->len, log, a->loc);
		es.from = a->hash;
		ChunkFormat->refs(data, a->len, audit_edge, &es);
		pfree(data);
		CHECK_FOR_INTERRUPTS();
	}

	/* and every count of references is the edges to it */
	*chunks = as.n;
	*unheld = 0;
	hash_seq_init(&seq, as.chunks);
	while ((a = hash_seq_search(&seq)) != NULL)
	{
		if (es.why == NULL && a->edges != a->refs)
			es.why = psprintf("chunk %s has %u references counted, and %llu edges to it",
							  hash_text(a->hash), a->refs,
							  (unsigned long long) a->edges);
		if (a->refs == 0 && a->roots == 0)
			(*unheld)++;
	}
	hash_destroy(as.chunks);
	*why = es.why;
	return es.why == NULL;
}
