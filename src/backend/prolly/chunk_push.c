/*-------------------------------------------------------------------------
 *
 * chunk_push.c
 *	  History pushed to the history store's tier, in bundles, and bundles
 *	  released there.
 *
 * A push of a bundle, named, of roots (chunks of the store, "pages")
 * copies every chunk the roots reach that the tier has not into the tier,
 * through the ChunkStore interface: children before the chunks naming
 * them, the bytes checked against their names as they are put.  The
 * bundle's row of pg_history.bundles then holds the roots in the tier (its
 * trigger's), and the store frees what nothing holds any more, all in the
 * push's transaction.  A push takes no root: the caller lets go of its
 * own (in the push's transaction, before it, for them to be freed with
 * it), and what anything else holds (a pointer, a root, a chunk naming
 * it) stays.
 *
 * So the push's commit is where history moves: a transaction before it,
 * and one whose snapshot is older, reads it in the store still (its bytes
 * kept until no snapshot could), and one after reads it there no more.  A
 * read of a chunk the store has not and the tier has is an error until it
 * is fetched back (prolly/chunkstore.h's "pages").
 *
 * A release of a bundle deletes its rows (the tier letting go of its
 * roots) and frees in the tier what nothing else holds, its packs' bytes
 * counted dead for VACUUM to remove.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_push.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "prolly/chunk_format.h"
#include "prolly/chunk_store.h"
#include "prolly/chunkstore.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "varatt.h"

/* the bytes of chunks put into the tier at once */
#define PUSH_BATCH_BYTES	(16 * 1024 * 1024)

typedef struct PushFrame
{
	uint8		hash[CHUNK_HASH_LEN];
	void	   *data;			/* once its children are on the stack */
	uint32		len;
} PushFrame;

typedef struct PushState
{
	ChunkStore *from;
	ChunkStore *to;
	PushFrame  *stack;
	int			depth;
	int			cap;
	/* the batch: chunks whose children are all in the tier, or in it */
	int			n;
	int			ncap;
	uint8	   *hashes;
	void	  **datas;
	uint32	   *lens;
	uint64		bytes;
	uint64		copied;
} PushState;

static void
push_frame(PushState *ps, const uint8 *hash)
{
	if (ps->depth == ps->cap)
	{
		ps->cap *= 2;
		ps->stack = repalloc_array(ps->stack, PushFrame, ps->cap);
	}
	memcpy(ps->stack[ps->depth].hash, hash, CHUNK_HASH_LEN);
	ps->stack[ps->depth].data = NULL;
	ps->stack[ps->depth].len = 0;
	ps->depth++;
}

static void
push_ref(void *arg, const uint8 *hash)
{
	push_frame((PushState *) arg, hash);
}

static void
flush_batch(PushState *ps)
{
	if (ps->n == 0)
		return;
	ps->copied += cs_put_many(ps->to, ps->n, ps->hashes,
							  (const void *const *) ps->datas, ps->lens);
	for (int i = 0; i < ps->n; i++)
		pfree(ps->datas[i]);
	ps->n = 0;
	ps->bytes = 0;
}

static void
batch_add(PushState *ps, const PushFrame *f)
{
	if (ps->n == ps->ncap)
	{
		ps->ncap *= 2;
		ps->hashes = repalloc(ps->hashes, (Size) ps->ncap * CHUNK_HASH_LEN);
		ps->datas = repalloc_array(ps->datas, void *, ps->ncap);
		ps->lens = repalloc_array(ps->lens, uint32, ps->ncap);
	}
	memcpy(ps->hashes + (Size) ps->n * CHUNK_HASH_LEN, f->hash, CHUNK_HASH_LEN);
	ps->datas[ps->n] = f->data;
	ps->lens[ps->n] = f->len;
	ps->n++;
	ps->bytes += f->len;
	if (ps->bytes >= PUSH_BATCH_BYTES)
		flush_batch(ps);
}

/*
 * Every chunk root reaches that the tier has not, copied into it: a walk
 * down, each chunk once (seen), one the tier has held there and not walked
 * below (the tier has what it references)
 */
static void
copy_reached(PushState *ps, HTAB *seen, const uint8 *root)
{
	push_frame(ps, root);
	while (ps->depth > 0)
	{
		PushFrame  *f = &ps->stack[ps->depth - 1];
		bool		found;

		CHECK_FOR_INTERRUPTS();
		if (f->data != NULL)
		{
			PushFrame	done = *f;

			ps->depth--;
			batch_add(ps, &done);
			continue;
		}
		(void) hash_search(seen, f->hash, HASH_ENTER, &found);
		if (found || cs_hold(ps->to, f->hash))
		{
			ps->depth--;
			continue;
		}
		f->data = cs_get(ps->from, f->hash, &f->len);
		if (f->data == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("the history store has no chunk %s to push",
							ChunkHashText(f->hash))));
		/* (an empty chunk's bytes, palloc'd for the batch to free) */
		if (f->len == 0)
		{
			pfree(f->data);
			f->data = palloc(1);
		}
		/* (f may move as the stack grows: its references found from a copy) */
		{
			void	   *data = f->data;
			uint32		len = f->len;

			ChunkFormat->refs(data, len, push_ref, ps);
		}
	}
}

/* the hashes of a bytea[] of roots, each a chunk's, none twice */
static uint8 *
array_hashes(ArrayType *arr, int *n)
{
	Datum	   *elems;
	bool	   *nulls;
	uint8	   *hashes;
	HASHCTL		ctl;
	HTAB	   *given;

	deconstruct_array(arr, BYTEAOID, -1, false, TYPALIGN_INT, &elems, &nulls, n);
	hashes = palloc((Size) Max(*n, 1) * CHUNK_HASH_LEN);
	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = CHUNK_HASH_LEN;
	ctl.hcxt = CurrentMemoryContext;
	given = hash_create("bundle roots", Max(*n, 16), &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < *n; i++)
	{
		bytea	   *b;
		bool		found;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("a bundle's root is no chunk's hash: null")));
		b = DatumGetByteaPP(elems[i]);
		if (VARSIZE_ANY_EXHDR(b) != CHUNK_HASH_LEN)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("a chunk's hash is %d bytes", CHUNK_HASH_LEN)));
		memcpy(hashes + (Size) i * CHUNK_HASH_LEN, VARDATA_ANY(b), CHUNK_HASH_LEN);
		(void) hash_search(given, VARDATA_ANY(b), HASH_ENTER, &found);
		if (found)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("root %s is given twice",
							ChunkHashText(hashes + (Size) i * CHUNK_HASH_LEN))));
	}
	hash_destroy(given);
	return hashes;
}

static void
check_pusher(void)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only a superuser may push or release history in the history store's tier")));
}

/* whether the bundle has rows */
static bool
bundle_exists(text *bundle)
{
	Oid			types[1] = {TEXTOID};
	Datum		values[1] = {PointerGetDatum(bundle)};

	if (SPI_execute_with_args("SELECT 1 FROM pg_history.bundles WHERE name = $1 LIMIT 1",
							  1, types, values, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not look up a bundle");
	return SPI_processed > 0;
}

/*
 * pg_history_push(bundle text, roots bytea[]) returns int8: the chunks
 * copied into the tier.  The caller lets go of its roots before it, in its
 * transaction, for them to be freed here with it; let go of after, they
 * stay unheld here until the background freer frees them.
 */
Datum
pg_history_push(PG_FUNCTION_ARGS)
{
	text	   *bundle = PG_GETARG_TEXT_PP(0);
	int			n;
	uint8	   *roots = array_hashes(PG_GETARG_ARRAYTYPE_P(1), &n);
	PushState	ps = {0};
	HASHCTL		ctl;
	HTAB	   *seen;
	Oid			types[2] = {TEXTOID, BYTEAARRAYOID};

	check_pusher();
	if (n == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a bundle has a root at least")));
	SPI_connect();
	if (bundle_exists(bundle))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("bundle \"%s\" is in the history store's tier already",
						text_to_cstring(bundle))));

	ps.from = ChunkStoreOpen("pages");
	ps.to = ChunkStoreOpen("tier");
	ps.cap = 64;
	ps.stack = palloc_array(PushFrame, ps.cap);
	ps.ncap = 64;
	ps.hashes = palloc((Size) ps.ncap * CHUNK_HASH_LEN);
	ps.datas = palloc_array(void *, ps.ncap);
	ps.lens = palloc_array(uint32, ps.ncap);
	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = CHUNK_HASH_LEN;
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("history push", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < n; i++)
		copy_reached(&ps, seen, roots + (Size) i * CHUNK_HASH_LEN);
	flush_batch(&ps);

	/*
	 * (the bundle's row holds its roots in the tier: its trigger.  Its name
	 * the key: a push of the same name at once waits, and fails.)
	 */
	{
		Datum		values[2] = {PointerGetDatum(bundle), PointerGetDatum(PG_GETARG_ARRAYTYPE_P(1))};

		if (SPI_execute_with_args("INSERT INTO pg_history.bundles (name, roots) VALUES ($1, $2)",
								  2, types, values, NULL, false, 0) != SPI_OK_INSERT)
			elog(ERROR, "could not insert a bundle");
	}
	SPI_finish();

	/*
	 * (what nothing holds here any more freed: the caller's roots, let go
	 * of by the caller, not; what others hold, a pointer, a root, a chunk
	 * referencing it, stays)
	 */
	(void) cs_free(ps.from, roots, n);
	PG_RETURN_INT64((int64) ps.copied);
}

/*
 * pg_history_release(bundle text) returns int8: the chunks freed in the
 * tier
 */
Datum
pg_history_release(PG_FUNCTION_ARGS)
{
	text	   *bundle = PG_GETARG_TEXT_PP(0);
	Oid			types[1] = {TEXTOID};
	Datum		values[1] = {PointerGetDatum(bundle)};
	uint8	   *roots;
	int			n;
	uint64		freed;

	check_pusher();
	SPI_connect();
	if (SPI_execute_with_args("DELETE FROM pg_history.bundles WHERE name = $1 RETURNING roots",
							  1, types, values, NULL, false, 0) != SPI_OK_DELETE_RETURNING)
		elog(ERROR, "could not delete a bundle");
	if (SPI_processed == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no bundle \"%s\" in the history store's tier",
						text_to_cstring(bundle))));
	{
		bool		isnull;
		Datum		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		uint8	   *found = array_hashes(DatumGetArrayTypeP(d), &n);

		/* (in the caller's memory: SPI_finish lets go of SPI's) */
		roots = SPI_palloc((Size) Max(n, 1) * CHUNK_HASH_LEN);
		memcpy(roots, found, (Size) n * CHUNK_HASH_LEN);
	}
	SPI_finish();
	freed = cs_free(ChunkStoreOpen("tier"), roots, n);
	PG_RETURN_INT64((int64) freed);
}
