/*-------------------------------------------------------------------------
 *
 * chunk_copy.c
 *	  Copying the chunks roots reach from one chunk store into another
 *	  (prolly/chunkstore.h's cs_copy_closure).
 *
 * A walk down from each root, each chunk once, a chunk's children read at
 * once (get_many) as the walk comes to it, one the target has held there
 * and not walked below (it has what that one references); the chunks put a
 * batch at a time (put_many), each after those it references, so a batch
 * lays its chunks out in the walk's order (a tier's pack: a chunk's
 * children together).  Memory: the chunks on the walk's way down, and a
 * batch.  The references are ChunkFormat's.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_copy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "prolly/chunk_format.h"
#include "prolly/chunk_store.h"
#include "prolly/chunkstore.h"
#include "utils/hsearch.h"

/* the bytes of chunks put into the target at once */
#define COPY_BATCH_BYTES	(16 * 1024 * 1024)

typedef struct CopyFrame
{
	uint8		hash[CHUNK_HASH_LEN];
	void	   *data;			/* its bytes, read with its siblings' */
	uint32		len;
	bool		expanded;		/* its children on the stack */
} CopyFrame;

typedef struct CopyState
{
	ChunkStore *from;
	ChunkStore *to;
	const char *verb;			/* the copy, for errors ("push", "fetch") */
	const char *source;			/* the store read from, for errors */
	CopyFrame  *stack;
	int			depth;
	int			cap;
	/* a chunk's children to read, at once */
	int			nkids;
	int			kidcap;
	uint8	   *kids;
	/* the batch: chunks whose children are all in the target, or in it */
	int			n;
	int			ncap;
	uint8	   *hashes;
	void	  **datas;
	uint32	   *lens;
	uint64		bytes;
	uint64		copied;
	HTAB	   *seen;			/* the chunks walked */
} CopyState;

static void
copy_frame(CopyState *ps, const uint8 *hash, void *data, uint32 len)
{
	if (ps->depth == ps->cap)
	{
		ps->cap *= 2;
		ps->stack = repalloc_array(ps->stack, CopyFrame, ps->cap);
	}
	memcpy(ps->stack[ps->depth].hash, hash, CHUNK_HASH_LEN);
	ps->stack[ps->depth].data = data;
	ps->stack[ps->depth].len = len;
	ps->stack[ps->depth].expanded = false;
	ps->depth++;
}

/* a child noted to read, unless seen already, or the target has it (held) */
static void
note_kid(void *arg, const uint8 *hash)
{
	CopyState  *ps = arg;
	bool		found;

	(void) hash_search(ps->seen, hash, HASH_FIND, &found);
	if (found)
		return;
	/* (held there: seen, so asked once, whatever order it is put in) */
	if (cs_hold(ps->to, hash))
	{
		(void) hash_search(ps->seen, hash, HASH_ENTER, NULL);
		return;
	}
	/* (a chunk named twice by one: read once) */
	for (int i = 0; i < ps->nkids; i++)
		if (memcmp(ps->kids + (Size) i * CHUNK_HASH_LEN, hash, CHUNK_HASH_LEN) == 0)
			return;
	if (ps->nkids == ps->kidcap)
	{
		ps->kidcap = Max(ps->kidcap * 2, 64);
		ps->kids = ps->kids ? repalloc(ps->kids, (Size) ps->kidcap * CHUNK_HASH_LEN) :
			palloc((Size) ps->kidcap * CHUNK_HASH_LEN);
	}
	memcpy(ps->kids + (Size) ps->nkids * CHUNK_HASH_LEN, hash, CHUNK_HASH_LEN);
	ps->nkids++;
}

/*
 * The chunks noted read from the source at once (in its own order: the
 * tier's, a few reads of each pack), each pushed with its bytes
 */
static void
read_kids(CopyState *ps)
{
	void	  **datas;
	uint32	   *lens;

	if (ps->nkids == 0)
		return;
	datas = palloc_array(void *, ps->nkids);
	lens = palloc_array(uint32, ps->nkids);
	cs_get_many(ps->from, ps->kids, ps->nkids, datas, lens);
	for (int i = 0; i < ps->nkids; i++)
	{
		const uint8 *hash = ps->kids + (Size) i * CHUNK_HASH_LEN;

		if (datas[i] == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("%s has no chunk %s to %s",
							ps->source, ChunkHashText(hash), ps->verb)));
		copy_frame(ps, hash, datas[i], lens[i]);
	}
	pfree(datas);
	pfree(lens);
	ps->nkids = 0;
}

static void
flush_batch(CopyState *ps)
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
batch_add(CopyState *ps, const CopyFrame *f)
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
	if (ps->bytes >= COPY_BATCH_BYTES)
		flush_batch(ps);
}

/*
 * Every chunk root reaches that the target has not, copied into it: a walk
 * down, each chunk once (seen as it comes to the top), one the target has
 * held there and not walked below (the target has what it references); a
 * chunk's children read at once as it is come to (a chunk named by two
 * not yet out may be read twice, the second, seen, let go of; one named
 * twice by one chunk is read once)
 */
static void
copy_reached(CopyState *ps, const uint8 *root)
{
	note_kid(ps, root);
	read_kids(ps);
	while (ps->depth > 0)
	{
		CopyFrame  *f = &ps->stack[ps->depth - 1];
		bool		found;

		CHECK_FOR_INTERRUPTS();
		if (f->expanded)
		{
			CopyFrame	done = *f;

			ps->depth--;
			batch_add(ps, &done);
			continue;
		}
		(void) hash_search(ps->seen, f->hash, HASH_ENTER, &found);
		if (found)
		{
			pfree(f->data);
			ps->depth--;
			continue;
		}
		f->expanded = true;
		/* (f may move as the stack grows: its references found from a copy) */
		{
			void	   *data = f->data;
			uint32		len = f->len;

			ChunkFormat->refs(data, len, note_kid, ps);
		}
		read_kids(ps);
	}
}

uint64
cs_copy_closure(ChunkStore *from, ChunkStore *to, const uint8 *roots, int n,
				const char *verb, const char *source)
{
	CopyState	ps = {0};
	HASHCTL		ctl;

	ps.from = from;
	ps.to = to;
	ps.verb = verb;
	ps.source = source;
	ps.cap = 64;
	ps.stack = palloc_array(CopyFrame, ps.cap);
	ps.ncap = 64;
	ps.hashes = palloc((Size) ps.ncap * CHUNK_HASH_LEN);
	ps.datas = palloc_array(void *, ps.ncap);
	ps.lens = palloc_array(uint32, ps.ncap);
	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = CHUNK_HASH_LEN;
	ctl.hcxt = CurrentMemoryContext;
	ps.seen = hash_create("chunk copy", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < n; i++)
		copy_reached(&ps, roots + (Size) i * CHUNK_HASH_LEN);
	flush_batch(&ps);
	hash_destroy(ps.seen);
	pfree(ps.stack);
	pfree(ps.hashes);
	pfree(ps.datas);
	pfree(ps.lens);
	if (ps.kids)
		pfree(ps.kids);
	return ps.copied;
}
