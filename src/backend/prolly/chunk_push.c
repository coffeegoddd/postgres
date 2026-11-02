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
 * A release of a bundle deletes its row (the tier letting go of its
 * roots) and frees in the tier what nothing else holds, its packs' bytes
 * counted dead for VACUUM to remove.  A fetch of a bundle copies what its
 * roots reach back into the store, the tier keeping its copy.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_push.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "prolly/chunk_format.h"
#include "prolly/chunk_store.h"
#include "prolly/chunk_tier.h"
#include "prolly/chunkstore.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"
#include "varatt.h"

/*
 * A bundle's manifest, an object in the tier (prolly/chunk_tier.h's
 * ChunkTierWriteManifest): its magic and version, its name's length and
 * its roots' number, then its name and its roots; numbers in network byte
 * order.  So the tier says what bundles it has without the server.
 */
#define MANIFEST_MAGIC		UINT64CONST(0x4c505259424e444c)	/* "LPRYBNDL" */
#define MANIFEST_VERSION	1

typedef struct ManifestHeader
{
	uint64		magic;
	uint32		version;
	uint32		namelen;
	uint32		nroots;
	uint32		unused;
} ManifestHeader;

/* the bytes the tier keeps */
static char *
encode_manifest(const char *name, const uint8 *roots, int n, size_t *len)
{
	ManifestHeader h = {0};
	size_t		namelen = strlen(name);
	char	   *buf;

	*len = sizeof(h) + namelen + (size_t) n * CHUNK_HASH_LEN;
	buf = palloc(*len);
	h.magic = pg_hton64(MANIFEST_MAGIC);
	h.version = pg_hton32(MANIFEST_VERSION);
	h.namelen = pg_hton32((uint32) namelen);
	h.nroots = pg_hton32((uint32) n);
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), name, namelen);
	memcpy(buf + sizeof(h) + namelen, roots, (size_t) n * CHUNK_HASH_LEN);
	return buf;
}

/* the bytes the tier kept: their name and roots; an error if not a manifest */
static void
decode_manifest(const char *data, size_t len, char **name, uint8 **roots, int *n)
{
	ManifestHeader h;
	uint32		namelen;

	if (len < sizeof(h))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a bundle's manifest of %zu bytes is too short", len)));
	memcpy(&h, data, sizeof(h));
	if (pg_ntoh64(h.magic) != MANIFEST_MAGIC || pg_ntoh32(h.version) != MANIFEST_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("an object under manifests/ is no bundle's manifest of this format")));
	namelen = pg_ntoh32(h.namelen);
	*n = (int) pg_ntoh32(h.nroots);
	if (*n <= 0 || namelen > len - sizeof(h) ||
		(uint64) *n * CHUNK_HASH_LEN != len - sizeof(h) - namelen ||
		memchr(data + sizeof(h), '\0', namelen) != NULL ||
		!pg_verifymbstr(data + sizeof(h), namelen, true))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a bundle's manifest of %zu bytes does not hold its name and %d roots", len, *n)));
	*name = pnstrdup(data + sizeof(h), namelen);
	*roots = palloc((Size) Max(*n, 1) * CHUNK_HASH_LEN);
	memcpy(*roots, data + sizeof(h) + namelen, (Size) *n * CHUNK_HASH_LEN);
}

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
	const char *verb;			/* "push" or "fetch", for errors */
	const char *source;			/* the store read from, for errors */
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
					 errmsg("%s has no chunk %s to %s",
							ps->source, ChunkHashText(f->hash), ps->verb)));
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
check_pusher(const char *fn)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only a superuser may push, release or fetch history in the history store's tier")));
	PreventCommandIfReadOnly(fn);
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
	Oid			types[3] = {TEXTOID, BYTEAARRAYOID, INT8OID};

	check_pusher("pg_history_push()");
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
	ps.verb = "push";
	ps.source = "the history store";
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
	 * (its manifest written to the tier, then the bundle's row, which holds
	 * its roots there, its trigger's, and names the manifest.  Its name the
	 * key: a push of the same name at once waits, and fails, its manifest
	 * an orphan.)
	 */
	{
		Relation	tier = relation_open(ChunkStoreTierLogRelid(), RowExclusiveLock);
		size_t		mlen;
		char	   *manifest = encode_manifest(text_to_cstring(bundle), roots, n, &mlen);
		int64		id = ChunkTierWriteManifest(tier, manifest, mlen);
		Datum		values[3] = {PointerGetDatum(bundle), PointerGetDatum(PG_GETARG_ARRAYTYPE_P(1)),
		Int64GetDatum(id)};

		relation_close(tier, NoLock);
		if (SPI_execute_with_args("INSERT INTO pg_history.bundles (name, roots, manifest) VALUES ($1, $2, $3)",
								  3, types, values, NULL, false, 0) != SPI_OK_INSERT)
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
	int64		manifest = 0;

	check_pusher("pg_history_release()");
	SPI_connect();
	if (SPI_execute_with_args("DELETE FROM pg_history.bundles WHERE name = $1 RETURNING roots, manifest",
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
		manifest = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
	}
	SPI_finish();
	freed = cs_free(ChunkStoreOpen("tier"), roots, n);
	{
		Relation	tier = relation_open(ChunkStoreTierLogRelid(), RowExclusiveLock);

		if (manifest <= 0 || manifest > PG_UINT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("bundle \"%s\" names no manifest (%lld)",
							text_to_cstring(bundle), (long long) manifest)));
		ChunkTierDropManifest(tier, (uint32) manifest);
		relation_close(tier, NoLock);
	}
	PG_RETURN_INT64((int64) freed);
}

/*
 * pg_history_fetch(bundle text) returns int8: the chunks copied back into
 * the store.  What the bundle's roots reach that the store has not is
 * copied from the tier into it (the walk a push's, the other way: each
 * chunk's bytes checked against its name as it is put), the tier keeping
 * its copy, the bundle as it was.  A fetch takes no root: the chunks it
 * put are held until its transaction ends, and the caller takes its roots
 * in it (those it does not are freed after, as any unheld).  A push undone
 * is a fetch, the caller's roots taken, and a release.  A release of the
 * bundle waits for the fetch (its row locked).  Two fetches (or pushes)
 * of bundles sharing chunks, in more than a batch, may wait on each
 * other's puts of them and deadlock, one cancelled, to be run again.
 */
Datum
pg_history_fetch(PG_FUNCTION_ARGS)
{
	text	   *bundle = PG_GETARG_TEXT_PP(0);
	Oid			types[1] = {TEXTOID};
	Datum		values[1] = {PointerGetDatum(bundle)};
	uint8	   *roots;
	int			n;
	PushState	ps = {0};
	HASHCTL		ctl;
	HTAB	   *seen;

	check_pusher("pg_history_fetch()");
	SPI_connect();

	/*
	 * (the bundle's row locked: a release of it waits for the fetch, whose
	 * walk the tier's freeing would cut short)
	 */
	if (SPI_execute_with_args("SELECT roots FROM pg_history.bundles WHERE name = $1 FOR KEY SHARE",
							  1, types, values, NULL, false, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not look up a bundle");
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

	ps.from = ChunkStoreOpen("tier");
	ps.to = ChunkStoreOpen("pages");
	ps.verb = "fetch";
	/* (none it reaches missing but by corruption: the tier keeps what it names) */
	ps.source = "the history store's tier";
	ps.cap = 64;
	ps.stack = palloc_array(PushFrame, ps.cap);
	ps.ncap = 64;
	ps.hashes = palloc((Size) ps.ncap * CHUNK_HASH_LEN);
	ps.datas = palloc_array(void *, ps.ncap);
	ps.lens = palloc_array(uint32, ps.ncap);
	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = CHUNK_HASH_LEN;
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("history fetch", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < n; i++)
		copy_reached(&ps, seen, roots + (Size) i * CHUNK_HASH_LEN);
	flush_batch(&ps);
	PG_RETURN_INT64((int64) ps.copied);
}

typedef struct BundlesListed
{
	ReturnSetInfo *rsinfo;
} BundlesListed;

static void
list_bundle(void *arg, const char *data, size_t len)
{
	BundlesListed *bl = arg;
	char	   *name;
	uint8	   *roots;
	int			n;
	Datum	   *elems;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	decode_manifest(data, len, &name, &roots, &n);
	elems = palloc_array(Datum, Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		bytea	   *h = palloc(VARHDRSZ + CHUNK_HASH_LEN);

		SET_VARSIZE(h, VARHDRSZ + CHUNK_HASH_LEN);
		memcpy(VARDATA(h), roots + (Size) i * CHUNK_HASH_LEN, CHUNK_HASH_LEN);
		elems[i] = PointerGetDatum(h);
	}
	values[0] = CStringGetTextDatum(name);
	values[1] = PointerGetDatum(construct_array(elems, n, BYTEAOID, -1, false, TYPALIGN_INT));
	tuplestore_putvalues(bl->rsinfo->setResult, bl->rsinfo->setDesc, values, nulls);
}

/*
 * pg_history_tier_bundles() returns setof (name text, roots bytea[]): the
 * bundles the tier's store has manifests of, read from it alone (a bundle
 * released and not yet VACUUMed there still; one aborted, until VACUUM
 * removes its manifest, too)
 */
Datum
pg_history_tier_bundles(PG_FUNCTION_ARGS)
{
	BundlesListed bl;
	Relation	tier;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only a superuser may list the bundles in the history store's tier")));
	InitMaterializedSRF(fcinfo, 0);
	bl.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	tier = relation_open(ChunkStoreTierLogRelid(), AccessShareLock);
	ChunkTierListManifests(tier, list_bundle, &bl);
	relation_close(tier, AccessShareLock);
	PG_RETURN_VOID();
}
