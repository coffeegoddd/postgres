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

bool
ChunkStorePut(Relation log, Relation idx, const uint8 *hash,
			  const void *data, uint32 len)
{
	ChunkIndexEntry e;
	RefList		refs = {0};

	PreventCommandIfReadOnly("put a chunk");
	check_hash(hash, data, len, NULL, 0);

	/*
	 * (a transaction id first, so that the commit of a put that finds the
	 * chunk there already flushes the WAL that put it, whoever wrote it)
	 */
	(void) GetCurrentTransactionId();

	/*
	 * (pinned before it is looked up, as every chunk it names is: a freer
	 * that marked one before then sees the pin, or this waits for it)
	 */
	ChunkPin(hash);
	if (ChunkIndex->lookup(idx, hash, &e))
		return false;

	/* a store holds a chunk only if it holds every chunk it references */
	ChunkFormat->refs(data, len, add_ref, &refs);
	for (int i = 0; i < refs.n; i++)
		ChunkPin(refs.hashes + i * CHUNK_HASH_LEN);
	for (int i = 0; i < refs.n; i++)
		if (!ChunkIndex->lookup(idx, refs.hashes + i * CHUNK_HASH_LEN, &e))
			ereport(ERROR,
					(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
					 errmsg("chunk %s references chunk %s, which the store does not have",
							hash_text(hash), hash_text(refs.hashes + i * CHUNK_HASH_LEN))));

	/* (an index is one log's: VACUUM counts the bytes it takes out there) */
	ChunkIndex->bind_log(idx, RelationGetRelid(log), ChunkLog->stamp(log));

	memset(&e, 0, sizeof(e));
	memcpy(e.hash, hash, CHUNK_HASH_LEN);
	e.len = len;
	e.loc = ChunkLog->append(log, data, len);
	{
		uint64		appended = e.loc;

		if (!ChunkIndex->insert(idx, &e))
		{
			/* (another put it meanwhile: the bytes appended are no chunk's) */
			if (len > 0)
				ChunkLog->mark_dead(log, appended, len);
			return false;
		}
	}

	/*
	 * each edge a reference of the chunk it names, in this transaction: an
	 * abort, or a crash, takes them back with the chunk
	 */
	for (int i = 0; i < refs.n; i++)
		if (!ChunkIndex->adjust(idx, refs.hashes + i * CHUNK_HASH_LEN, 1, 0, &e))
			ereport(ERROR,
					(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
					 errmsg("chunk %s references chunk %s, which the store does not have",
							hash_text(hash), hash_text(refs.hashes + i * CHUNK_HASH_LEN))));
	if (refs.hashes)
		pfree(refs.hashes);
	return true;
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
