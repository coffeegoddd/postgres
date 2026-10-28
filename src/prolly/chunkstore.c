/*
 * chunkstore.c
 *	  Chunk lifetime: reference counts, roots, pins, freeing, closure copy
 *	  and the invariant audit.  Backend independent.
 */
#include "chunkstore.h"

static bool
is_dead(const ChunkMeta *m)
{
	return m->refs == 0 && m->roots == 0 && m->pins == 0;
}

typedef struct HashVec
{
	PHash	   *v;
	size_t		n;
	size_t		cap;
} HashVec;

static void
hashvec_push(HashVec *hv, const PHash *h)
{
	px_grow((void **) &hv->v, &hv->cap, hv->n + 1, sizeof(PHash));
	hv->v[hv->n++] = *h;
}

typedef struct ChildCheck
{
	ChunkStore *cs;
	bool		ok;
	PHash		missing;
} ChildCheck;

static void
child_check_fn(void *arg, const PHash *h, bool strong)
{
	ChildCheck *c = arg;

	if (strong && c->ok && !c->cs->ops->has(c->cs, h))
	{
		c->ok = false;
		c->missing = *h;
	}
}

static void
incref_fn(void *arg, const PHash *h, bool strong)
{
	ChunkStore *cs = arg;

	if (strong)
	{
		cs->ops->meta(cs, h)->refs++;
		cs->stats.ref_updates++;
	}
}

int
cs_put(ChunkStore *cs, const uint8_t *data, size_t len, PHash *out,
	   PHash *err_hash)
{
	ChildCheck	cc = {cs, true, {{0}}};

	phash_compute(data, len, out);
	cs->stats.puts++;
	if (cs->ops->has(cs, out))
		return CS_OK;
	if (!chunk_refs(data, len, child_check_fn, &cc))
	{
		*err_hash = *out;
		return CS_ERR_CORRUPT;
	}
	if (!cc.ok)
	{
		*err_hash = cc.missing;
		return CS_ERR_MISSING;
	}
	cs->ops->put(cs, out, data, len);
	cs->stats.new_chunks++;
	cs->stats.new_bytes += len;
	/* only a newly stored chunk adds edges; a duplicate is the same chunk */
	chunk_refs(data, len, incref_fn, cs);
	return CS_OK;
}

typedef struct FreeCtx
{
	ChunkStore *cs;
	HashVec    *work;
} FreeCtx;

static void
decref_fn(void *arg, const PHash *h, bool strong)
{
	FreeCtx    *f = arg;
	ChunkMeta  *m;

	if (!strong)
		return;
	m = f->cs->ops->meta(f->cs, h);
	m->refs--;
	f->cs->stats.ref_updates++;
	if (is_dead(m))
		hashvec_push(f->work, h);
}

/* Frees h if nothing holds it, then whatever that leaves unheld. */
static void
free_cascade(ChunkStore *cs, const PHash *start)
{
	HashVec		work = {0};
	FreeCtx		f = {cs, &work};

	hashvec_push(&work, start);
	while (work.n > 0)
	{
		PHash		h = work.v[--work.n];
		ChunkMeta  *m = cs->ops->meta(cs, &h);
		const uint8_t *data;
		size_t		len;

		if (m == NULL || !is_dead(m))
			continue;
		data = cs->ops->get(cs, &h, &len);
		chunk_refs(data, len, decref_fn, &f);
		cs->stats.frees++;
		cs->stats.freed_bytes += len;
		cs->ops->del(cs, &h);
	}
	free(work.v);
}

static ChunkMeta *
must_meta(ChunkStore *cs, const PHash *h)
{
	ChunkMeta  *m = cs->ops->meta(cs, h);

	if (m == NULL)
	{
		char		hex[PHASH_HEX_LEN + 1];

		phash_hex(h, hex);
		fprintf(stderr, "chunkstore: hold on absent chunk %s\n", hex);
		abort();
	}
	return m;
}

void
cs_ref_root(ChunkStore *cs, const PHash *h)
{
	must_meta(cs, h)->roots++;
}

void
cs_unref_root(ChunkStore *cs, const PHash *h)
{
	ChunkMeta  *m = must_meta(cs, h);

	if (m->roots == 0)
		abort();
	if (--m->roots == 0 && is_dead(m))
		free_cascade(cs, h);
}

void
cs_pin(ChunkStore *cs, const PHash *h)
{
	must_meta(cs, h)->pins++;
}

void
cs_unpin(ChunkStore *cs, const PHash *h)
{
	ChunkMeta  *m = must_meta(cs, h);

	if (m->pins == 0)
		abort();
	if (--m->pins == 0 && is_dead(m))
		free_cascade(cs, h);
}

void
pinset_init(PinSet *ps, ChunkStore *cs)
{
	memset(ps, 0, sizeof(*ps));
	ps->cs = cs;
}

void
pinset_add(PinSet *ps, const PHash *h)
{
	cs_pin(ps->cs, h);
	px_grow((void **) &ps->v, &ps->cap, ps->n + 1, sizeof(PHash));
	ps->v[ps->n++] = *h;
}

void
pinset_release(PinSet *ps)
{
	/* newest first, so parents go before the children they hold */
	while (ps->n > 0)
		cs_unpin(ps->cs, &ps->v[--ps->n]);
}

void
pinset_free(PinSet *ps)
{
	pinset_release(ps);
	free(ps->v);
	ps->v = NULL;
	ps->cap = 0;
}

static void
collect_fn(void *arg, const PHash *h)
{
	hashvec_push(arg, h);
}

size_t
cs_collect_orphans(ChunkStore *cs)
{
	HashVec		all = {0};
	uint64_t	before = cs->stats.frees;

	cs->ops->foreach(cs, collect_fn, &all);
	for (size_t i = 0; i < all.n; i++)
	{
		ChunkMeta  *m = cs->ops->meta(cs, &all.v[i]);

		if (m && is_dead(m))
			free_cascade(cs, &all.v[i]);
	}
	free(all.v);
	return (size_t) (cs->stats.frees - before);
}

static void
strong_fn(void *arg, const PHash *h, bool strong)
{
	if (strong)
		hashvec_push(arg, h);
}

int
cs_copy_closure(ChunkStore *src, PinSet *dst, const PHash *h, bool verify,
				PHash *err_hash)
{
	const uint8_t *data;
	size_t		len;
	HashVec		kids = {0};
	PHash		got;
	int			rc = CS_OK;

	if (cs_has(dst->cs, h))
	{
		pinset_add(dst, h);
		return CS_OK;
	}
	data = cs_get(src, h, &len);
	if (data == NULL)
	{
		*err_hash = *h;
		return CS_ERR_MISSING;
	}
	if (verify)
	{
		phash_compute(data, len, &got);
		if (!phash_eq(&got, h))
		{
			*err_hash = *h;
			return CS_ERR_CORRUPT;
		}
	}
	if (!chunk_refs(data, len, strong_fn, &kids))
	{
		*err_hash = *h;
		return CS_ERR_CORRUPT;
	}
	for (size_t i = 0; i < kids.n && rc == CS_OK; i++)
		rc = cs_copy_closure(src, dst, &kids.v[i], verify, err_hash);
	free(kids.v);
	if (rc != CS_OK)
		return rc;
	rc = cs_put(dst->cs, data, len, &got, err_hash);
	if (rc == CS_OK)
		pinset_add(dst, &got);
	return rc;
}

typedef struct Audit
{
	ChunkStore *cs;
	PHashMap   *expect;
	bool		ok;
	PHash		bad;
} Audit;

static void
audit_ref_fn(void *arg, const PHash *h, bool strong)
{
	Audit	   *a = arg;
	bool		created;

	if (!strong)
		return;
	if (!a->cs->ops->has(a->cs, h) && a->ok)
	{
		a->ok = false;
		a->bad = *h;
	}
	(*phmap_ref(a->expect, h, &created))++;
}

bool
cs_check_invariants(ChunkStore *cs, size_t *orphans, char *why, size_t whylen)
{
	HashVec		all = {0};
	PHashMap	expect;
	Audit		a;
	bool		ok = true;
	char		hex[PHASH_HEX_LEN + 1];

	phmap_init(&expect);
	a.cs = cs;
	a.expect = &expect;
	a.ok = true;
	*orphans = 0;
	cs->ops->foreach(cs, collect_fn, &all);
	for (size_t i = 0; i < all.n && ok; i++)
	{
		size_t		len;
		const uint8_t *data = cs->ops->get(cs, &all.v[i], &len);

		if (!chunk_refs(data, len, audit_ref_fn, &a))
		{
			phash_hex(&all.v[i], hex);
			snprintf(why, whylen, "chunk %s does not parse", hex);
			ok = false;
		}
		else if (!a.ok)
		{
			phash_hex(&a.bad, hex);
			snprintf(why, whylen, "strong reference to absent chunk %s", hex);
			ok = false;
		}
	}
	for (size_t i = 0; i < all.n && ok; i++)
	{
		ChunkMeta  *m = cs->ops->meta(cs, &all.v[i]);
		uint64_t   *e = phmap_find(&expect, &all.v[i]);
		uint64_t	want = e ? *e : 0;

		if (m->refs != want)
		{
			phash_hex(&all.v[i], hex);
			snprintf(why, whylen, "chunk %s refs %llu, expected %llu", hex,
					 (unsigned long long) m->refs, (unsigned long long) want);
			ok = false;
		}
		if (is_dead(m))
			(*orphans)++;
	}
	phmap_free(&expect);
	free(all.v);
	return ok;
}
