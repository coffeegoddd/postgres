/*
 * packstore.c
 *	  A ChunkStore on a plain-storage remote: see packstore.h.
 */
#include "packstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../phash.h"
#include "../prolly_util.h"

#define PACK_HOLD	(UINT64_C(1) << 40)		/* the remote's hold on a packed chunk */
#define COALESCE_GAP (64 * 1024)	/* read across gaps this small */
#define INDEX_ENT	(PHASH_LEN + 8 + 4)

typedef struct Pack
{
	char		name[PHASH_HEX_LEN + 16];
	uint64_t	nchunks;
	uint64_t	data_bytes;
	uint64_t	index_off;
	uint64_t	index_len;
} Pack;

typedef struct Ent
{
	PHash		h;
	int64_t		pack;			/* -1: pending, not yet uploaded */
	uint64_t	off;
	uint32_t	len;
	uint8_t    *data;			/* cached or pending bytes */
	bool		dead;
	ChunkMeta	meta;
} Ent;

typedef struct PackStore
{
	ChunkStore	base;
	Remote	   *remote;
	PHashMap	map;			/* hash -> index into ents */
	Ent		   *ents;
	size_t		nents;
	size_t		entcap;
	Pack	   *packs;
	size_t		npacks;
	size_t		packcap;
	size_t	   *pending;		/* ents not uploaded, in put order */
	size_t		npending;
	size_t		pendcap;
	uint64_t	bytes;
	char		etag[REMOTE_ETAG_LEN];
	bool		has_manifest;
	PackStoreStats stats;
} PackStore;

static double
now_secs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
put_le64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t) (v >> (8 * i));
}

static void
put_le32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t) (v >> (8 * i));
}

static uint64_t
get_le64(const uint8_t *p)
{
	uint64_t	v = 0;

	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static uint32_t
get_le32(const uint8_t *p)
{
	uint32_t	v = 0;

	for (int i = 3; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static bool
parse_hex(const char *s, PHash *h)
{
	for (int i = 0; i < PHASH_LEN; i++)
	{
		unsigned	b;

		if (sscanf(s + 2 * i, "%2x", &b) != 1)
			return false;
		h->b[i] = (uint8_t) b;
	}
	return true;
}

static Ent *
find(PackStore *ps, const PHash *h)
{
	uint64_t   *v = phmap_find(&ps->map, h);

	if (v == NULL || ps->ents[*v].dead)
		return NULL;
	return &ps->ents[*v];
}

static Ent *
add(PackStore *ps, const PHash *h)
{
	bool		created;
	uint64_t   *v = phmap_ref(&ps->map, h, &created);
	Ent		   *e;

	if (!created)
	{
		e = &ps->ents[*v];
		e->dead = false;
		return e;
	}
	px_grow((void **) &ps->ents, &ps->entcap, ps->nents + 1, sizeof(Ent));
	*v = ps->nents;
	e = &ps->ents[ps->nents++];
	memset(e, 0, sizeof(*e));
	e->h = *h;
	return e;
}

static char *
pack_key(const char *name)
{
	char	   *k = px_malloc(strlen(name) + 16);

	sprintf(k, "packs/%s.pack", name);
	return k;
}

/* ------------------------------------------------------------- reading */

static const uint8_t *
ps_get(ChunkStore *cs, const PHash *h, size_t *len)
{
	PackStore  *ps = (PackStore *) cs;
	Ent		   *e = find(ps, h);

	if (e == NULL)
		return NULL;
	if (e->data == NULL)
	{
		char	   *key = pack_key(ps->packs[e->pack].name);
		size_t		got;

		e->data = remote_get(ps->remote, key, e->off, e->len, &got, NULL);
		free(key);
		if (e->data == NULL || got != e->len)
		{
			fprintf(stderr, "packstore: could not read a chunk from the remote\n");
			abort();
		}
		ps->stats.chunk_gets++;
		ps->stats.range_gets++;
		/* (ents may have moved: find again) */
		e = find(ps, h);
	}
	*len = e->len;
	return e->data;
}

typedef struct Want
{
	size_t		ent;
	int64_t		pack;
	uint64_t	off;
	uint32_t	len;
} Want;

static int
want_cmp(const void *a, const void *b)
{
	const Want *x = a,
			   *y = b;

	if (x->pack != y->pack)
		return x->pack < y->pack ? -1 : 1;
	return x->off < y->off ? -1 : x->off > y->off;
}

/*
 * Reads every chunk of hs not yet read: ranges of one pack closer than
 * COALESCE_GAP become one request, and the requests go out in parallel.
 */
static bool
ps_get_many(ChunkStore *cs, const PHash *hs, size_t n, const uint8_t **out,
			size_t *lens)
{
	PackStore  *ps = (PackStore *) cs;
	Want	   *w = px_malloc((n ? n : 1) * sizeof(Want));
	size_t		nw = 0;
	RemoteRange *rr;
	size_t		nr = 0;
	size_t	   *first;
	bool		ok;

	for (size_t i = 0; i < n; i++)
	{
		Ent		   *e = find(ps, &hs[i]);

		if (e && e->data == NULL && e->pack >= 0)
			w[nw++] = (Want) {(size_t) (e - ps->ents), e->pack, e->off, e->len};
	}
	if (nw > 1)
		qsort(w, nw, sizeof(Want), want_cmp);
	rr = px_malloc((nw ? nw : 1) * sizeof(RemoteRange));
	first = px_malloc((nw ? nw : 1) * sizeof(size_t));
	for (size_t i = 0; i < nw;)
	{
		size_t		j = i + 1;
		uint64_t	end = w[i].off + w[i].len;

		while (j < nw && w[j].pack == w[i].pack && w[j].off <= end + COALESCE_GAP)
		{
			if (w[j].off + w[j].len > end)
				end = w[j].off + w[j].len;
			j++;
		}
		rr[nr] = (RemoteRange) {pack_key(ps->packs[w[i].pack].name), w[i].off,
		end - w[i].off, NULL, 0};
		first[nr++] = i;
		i = j;
	}
	ok = remote_get_many(ps->remote, rr, nr);
	ps->stats.range_gets += nr;
	for (size_t k = 0; k < nr; k++)
	{
		size_t		lo = first[k],
					hi = k + 1 < nr ? first[k + 1] : nw;

		for (size_t i = lo; i < hi && rr[k].data; i++)
		{
			Ent		   *e = &ps->ents[w[i].ent];

			e->data = px_memdup(rr[k].data + (w[i].off - rr[k].off), w[i].len);
			ps->stats.chunk_gets++;
		}
		free(rr[k].data);
		free((char *) rr[k].key);
	}
	free(rr);
	free(first);
	free(w);
	for (size_t i = 0; i < n; i++)
	{
		Ent		   *e = find(ps, &hs[i]);

		out[i] = e ? e->data : NULL;
		lens[i] = e ? e->len : 0;
	}
	return ok;
}

static bool
ps_has(ChunkStore *cs, const PHash *h)
{
	return find((PackStore *) cs, h) != NULL;
}

static ChunkMeta *
ps_meta(ChunkStore *cs, const PHash *h)
{
	Ent		   *e = find((PackStore *) cs, h);

	return e ? &e->meta : NULL;
}

/* ------------------------------------------------------------- writing */

static bool
ps_put(ChunkStore *cs, const PHash *h, const uint8_t *data, size_t len)
{
	PackStore  *ps = (PackStore *) cs;
	Ent		   *e;

	if (find(ps, h))
		return false;
	e = add(ps, h);
	e->pack = -1;
	e->len = (uint32_t) len;
	free(e->data);
	e->data = px_memdup(data, len);
	memset(&e->meta, 0, sizeof(e->meta));
	px_grow((void **) &ps->pending, &ps->pendcap, ps->npending + 1, sizeof(size_t));
	ps->pending[ps->npending++] = (size_t) (e - ps->ents);
	ps->bytes += len;
	return true;
}

static void
ps_del(ChunkStore *cs, const PHash *h)
{
	PackStore  *ps = (PackStore *) cs;
	Ent		   *e = find(ps, h);

	/* (a packed chunk stays on the remote; a pending one isn't sent) */
	if (e == NULL)
		return;
	e->dead = true;
	if (e->pack < 0)
		ps->bytes -= e->len;
}

static void
ps_foreach(ChunkStore *cs, ChunkVisitFn fn, void *arg)
{
	PackStore  *ps = (PackStore *) cs;

	for (size_t i = 0; i < ps->nents; i++)
		if (!ps->ents[i].dead)
			fn(arg, &ps->ents[i].h);
}

static size_t
ps_count(ChunkStore *cs)
{
	PackStore  *ps = (PackStore *) cs;
	size_t		n = 0;

	for (size_t i = 0; i < ps->nents; i++)
		n += !ps->ents[i].dead;
	return n;
}

static uint64_t
ps_bytes(ChunkStore *cs)
{
	return ((PackStore *) cs)->bytes;
}

static void
ps_destroy(ChunkStore *cs)
{
	PackStore  *ps = (PackStore *) cs;

	for (size_t i = 0; i < ps->nents; i++)
		free(ps->ents[i].data);
	free(ps->ents);
	free(ps->packs);
	free(ps->pending);
	phmap_free(&ps->map);
	free(ps);
}

static const ChunkStoreOps packstore_ops = {
	.get = ps_get,
	.put = ps_put,
	.has = ps_has,
	.meta = ps_meta,
	.del = ps_del,
	.foreach = ps_foreach,
	.count = ps_count,
	.bytes = ps_bytes,
	.destroy = ps_destroy,
	.get_many = ps_get_many,
};

/* ------------------------------------------------------------ manifest */

static char *
manifest_text(PackStore *ps, const PHash *state, size_t *len)
{
	size_t		cap = 128 + ps->npacks * 160;
	char	   *s = px_malloc(cap);
	char		hex[PHASH_HEX_LEN + 1];
	size_t		n;

	phash_hex(state, hex);
	n = (size_t) snprintf(s, cap, "prolly-remote 1\nstate %s\n", hex);
	for (size_t i = 0; i < ps->npacks; i++)
		n += (size_t) snprintf(s + n, cap - n, "pack %s %llu %llu %llu %llu\n",
							   ps->packs[i].name,
							   (unsigned long long) ps->packs[i].nchunks,
							   (unsigned long long) ps->packs[i].data_bytes,
							   (unsigned long long) ps->packs[i].index_off,
							   (unsigned long long) ps->packs[i].index_len);
	*len = n;
	return s;
}

ChunkStore *
packstore_open(Remote *r, PHash *state, char *err, size_t errlen)
{
	PackStore  *ps = calloc(1, sizeof(PackStore));
	double		t0 = now_secs();
	size_t		len;
	uint8_t    *m;
	RemoteRange *rr;

	ps->base.ops = &packstore_ops;
	ps->remote = r;
	phmap_init(&ps->map);
	memset(state, 0, sizeof(*state));
	m = remote_get(r, "manifest", 0, 0, &len, ps->etag);
	if (m != NULL)
	{
		char	   *text = px_realloc(m, len + 1);
		char	   *line,
				   *save;

		text[len] = '\0';
		ps->has_manifest = true;
		for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		{
			Pack		p;
			unsigned long long a,
						b,
						c,
						d;

			if (strncmp(line, "state ", 6) == 0)
				parse_hex(line + 6, state);
			else if (sscanf(line, "pack %80s %llu %llu %llu %llu", p.name, &a, &b, &c, &d) == 5)
			{
				p.nchunks = a;
				p.data_bytes = b;
				p.index_off = c;
				p.index_len = d;
				px_grow((void **) &ps->packs, &ps->packcap, ps->npacks + 1, sizeof(Pack));
				ps->packs[ps->npacks++] = p;
			}
		}
		free(text);
	}

	/* every pack's index, in parallel */
	rr = px_malloc((ps->npacks ? ps->npacks : 1) * sizeof(RemoteRange));
	for (size_t i = 0; i < ps->npacks; i++)
		rr[i] = (RemoteRange) {pack_key(ps->packs[i].name), ps->packs[i].index_off,
		ps->packs[i].index_len, NULL, 0};
	if (!remote_get_many(r, rr, ps->npacks))
	{
		snprintf(err, errlen, "could not read the remote's pack indexes");
		for (size_t i = 0; i < ps->npacks; i++)
		{
			free(rr[i].data);
			free((char *) rr[i].key);
		}
		free(rr);
		ps_destroy(&ps->base);
		return NULL;
	}
	for (size_t i = 0; i < ps->npacks; i++)
	{
		for (uint64_t k = 0; k < ps->packs[i].nchunks; k++)
		{
			const uint8_t *p = rr[i].data + k * INDEX_ENT;
			PHash		h;
			Ent		   *e;

			memcpy(h.b, p, PHASH_LEN);
			e = add(ps, &h);
			e->pack = (int64_t) i;
			e->off = get_le64(p + PHASH_LEN);
			e->len = get_le32(p + PHASH_LEN + 8);
			e->meta.roots = PACK_HOLD;
			ps->bytes += e->len;
		}
		ps->stats.index_bytes += rr[i].got;
		free(rr[i].data);
		free((char *) rr[i].key);
	}
	free(rr);
	ps->stats.packs = ps->npacks;
	ps->stats.open_secs = now_secs() - t0;
	return &ps->base;
}

PackStoreStats *
packstore_stats(ChunkStore *cs)
{
	return &((PackStore *) cs)->stats;
}

Remote *
packstore_remote(ChunkStore *cs)
{
	return ((PackStore *) cs)->remote;
}

int
packstore_flush(ChunkStore *cs, const PHash *state)
{
	PackStore  *ps = (PackStore *) cs;
	double		t0 = now_secs();
	uint64_t	data_bytes = 0;
	size_t		live = 0;
	uint8_t    *buf;
	uint64_t	off = 0;
	PHash		ph;
	Pack		p;
	char	   *key;
	char	   *mtext;
	size_t		mlen;
	char		etag[REMOTE_ETAG_LEN];
	int			rc;

	for (size_t i = 0; i < ps->npending; i++)
	{
		Ent		   *e = &ps->ents[ps->pending[i]];

		if (!e->dead)
		{
			data_bytes += e->len;
			live++;
		}
	}
	if (live > 0)
	{
		/* the chunks in put order, then the index */
		buf = px_malloc(data_bytes + live * INDEX_ENT);
		for (size_t i = 0; i < ps->npending; i++)
		{
			Ent		   *e = &ps->ents[ps->pending[i]];

			if (e->dead)
				continue;
			memcpy(buf + off, e->data, e->len);
			off += e->len;
		}
		off = 0;
		for (size_t i = 0, k = 0; i < ps->npending; i++)
		{
			Ent		   *e = &ps->ents[ps->pending[i]];
			uint8_t    *q;

			if (e->dead)
				continue;
			q = buf + data_bytes + (k++) * INDEX_ENT;
			memcpy(q, e->h.b, PHASH_LEN);
			put_le64(q + PHASH_LEN, off);
			put_le32(q + PHASH_LEN + 8, e->len);
			off += e->len;
		}
		phash_compute(buf, data_bytes + live * INDEX_ENT, &ph);
		phash_hex(&ph, p.name);
		p.nchunks = live;
		p.data_bytes = data_bytes;
		p.index_off = data_bytes;
		p.index_len = live * INDEX_ENT;
		ps->stats.pack_build_secs += now_secs() - t0;
		key = pack_key(p.name);
		rc = remote_put(ps->remote, key, buf, data_bytes + live * INDEX_ENT);
		free(key);
		free(buf);
		if (rc != 0)
			return -1;
		ps->stats.packs_written++;
		ps->stats.pack_bytes_written += data_bytes + live * INDEX_ENT;
		px_grow((void **) &ps->packs, &ps->packcap, ps->npacks + 1, sizeof(Pack));
		ps->packs[ps->npacks++] = p;
	}

	mtext = manifest_text(ps, state, &mlen);
	rc = remote_put_if(ps->remote, "manifest", (const uint8_t *) mtext, mlen,
					   ps->has_manifest ? ps->etag : NULL, etag);
	free(mtext);
	if (rc != 0)
	{
		if (live > 0)
			ps->npacks--;		/* (the uploaded pack is garbage now) */
		return rc;
	}
	strcpy(ps->etag, etag);
	ps->has_manifest = true;

	/* the pending chunks are in the new pack, held by the remote */
	off = 0;
	for (size_t i = 0; i < ps->npending; i++)
	{
		Ent		   *e = &ps->ents[ps->pending[i]];

		if (e->dead)
			continue;
		e->pack = (int64_t) ps->npacks - 1;
		e->off = off;
		off += e->len;
		e->meta.roots += PACK_HOLD;
	}
	ps->npending = 0;
	return 0;
}

bool
packstore_stream(ChunkStore *cs, PackChunkFn fn, void *arg)
{
	PackStore  *ps = (PackStore *) cs;

	for (size_t i = 0; i < ps->npacks; i++)
	{
		char	   *key = pack_key(ps->packs[i].name);
		size_t		got;
		uint8_t    *data = remote_get(ps->remote, key, 0, 0, &got, NULL);
		const uint8_t *idx;

		free(key);
		if (data == NULL || got != ps->packs[i].data_bytes + ps->packs[i].index_len)
		{
			free(data);
			return false;
		}
		idx = data + ps->packs[i].index_off;
		for (uint64_t k = 0; k < ps->packs[i].nchunks; k++)
		{
			const uint8_t *q = idx + k * INDEX_ENT;
			PHash		h;

			memcpy(h.b, q, PHASH_LEN);
			fn(arg, &h, data + get_le64(q + PHASH_LEN), get_le32(q + PHASH_LEN + 8));
		}
		ps->stats.chunk_gets += ps->packs[i].nchunks;
		ps->stats.range_gets++;
		free(data);
	}
	return true;
}
