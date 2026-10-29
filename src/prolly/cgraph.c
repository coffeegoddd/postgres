/*
 * cgraph.c
 *	  A commit graph in a prolly map.
 *
 * Key: 8-byte big-endian generation, then the commit hash.
 * Value: uvarint flags, uvarint nparents, (parent hash, uvarint generation)...,
 *		  uvarint nseams, segment hash...
 * The map holds nothing strongly: commits and segments are held by
 * whoever keeps them.
 */
#include "cgraph.h"

#define CG_KEY_LEN (8 + PHASH_LEN)

typedef struct ProllyGraph
{
	CommitGraph g;
	ProllyCtx  *ctx;
	PHash		root;
	bool		held;
} ProllyGraph;

static void
key_encode(uint8_t *k, uint64_t gen, const PHash *c)
{
	for (int i = 0; i < 8; i++)
		k[i] = (uint8_t) (gen >> (56 - 8 * i));
	memcpy(k + 8, c->b, PHASH_LEN);
}

static bool
key_decode(const uint8_t *k, size_t len, uint64_t *gen, PHash *c)
{
	if (len != CG_KEY_LEN)
		return false;
	*gen = 0;
	for (int i = 0; i < 8; i++)
		*gen = (*gen << 8) | k[i];
	memcpy(c->b, k + 8, PHASH_LEN);
	return true;
}

void
cg_edge_encode(const CgEdge *e, Buf *out)
{
	buf_append_uvarint(out, e->flags);
	buf_append_uvarint(out, e->nparents);
	for (uint32_t i = 0; i < e->nparents; i++)
	{
		buf_append(out, e->parents[i].b, PHASH_LEN);
		buf_append_uvarint(out, e->parent_gen[i]);
	}
	buf_append_uvarint(out, e->nseams);
	for (uint32_t i = 0; i < e->nseams; i++)
		buf_append(out, e->seams[i].b, PHASH_LEN);
}

bool
cg_edge_decode(const uint8_t *p, size_t len, CgEdge *out)
{
	const uint8_t *end = p + len;
	uint64_t	v;

	memset(out, 0, sizeof(*out));
	if (!read_uvarint(&p, end, &v))
		return false;
	out->flags = (uint32_t) v;
	if (!read_uvarint(&p, end, &v) || v > CG_MAX_PARENTS)
		return false;
	out->nparents = (uint32_t) v;
	for (uint32_t i = 0; i < out->nparents; i++)
	{
		if ((size_t) (end - p) < PHASH_LEN)
			return false;
		memcpy(out->parents[i].b, p, PHASH_LEN);
		p += PHASH_LEN;
		if (!read_uvarint(&p, end, &out->parent_gen[i]))
			return false;
	}
	if (!read_uvarint(&p, end, &v) || v > CG_MAX_SEAMS)
		return false;
	out->nseams = (uint32_t) v;
	for (uint32_t i = 0; i < out->nseams; i++)
	{
		if ((size_t) (end - p) < PHASH_LEN)
			return false;
		memcpy(out->seams[i].b, p, PHASH_LEN);
		p += PHASH_LEN;
	}
	return p == end;
}

bool
cg_edge_eq(const CgEdge *a, const CgEdge *b)
{
	Buf			x = BUF_INIT,
				y = BUF_INIT;
	bool		eq;

	cg_edge_encode(a, &x);
	cg_edge_encode(b, &y);
	eq = bytes_eq(x.data, x.len, y.data, y.len);
	buf_free(&x);
	buf_free(&y);
	return eq;
}

static int
pg_get(CommitGraph *g, uint64_t gen, const PHash *commit, CgEdge *out)
{
	ProllyGraph *pg = (ProllyGraph *) g;
	uint8_t		k[CG_KEY_LEN];
	const uint8_t *v;
	size_t		vl;
	int			rc;

	key_encode(k, gen, commit);
	rc = prolly_get(pg->ctx, &pg->root, k, sizeof(k), &v, &vl);
	if (rc != PROLLY_OK)
		return rc;
	if (!cg_edge_decode(v, vl, out))
	{
		pg->ctx->err_hash = pg->root;
		return PROLLY_ERR_CORRUPT;
	}
	return PROLLY_OK;
}

static int
pg_apply(CommitGraph *g, const CgPut *puts, size_t n)
{
	ProllyGraph *pg = (ProllyGraph *) g;
	ProllyEdit *e;
	uint8_t    *keys = px_malloc(n * CG_KEY_LEN);
	Buf		   *vals = px_malloc(n * sizeof(Buf));
	PHash		out,
				old = pg->root;
	int			rc;

	if (n == 0)
	{
		free(keys);
		free(vals);
		return PROLLY_OK;
	}
	e = px_malloc(n * sizeof(ProllyEdit));
	memset(e, 0, n * sizeof(ProllyEdit));
	for (size_t i = 0; i < n; i++)
	{
		vals[i] = (Buf) BUF_INIT;
		key_encode(keys + i * CG_KEY_LEN, puts[i].gen, &puts[i].commit);
		e[i].key = keys + i * CG_KEY_LEN;
		e[i].klen = CG_KEY_LEN;
		e[i].del = puts[i].edge == NULL;
		if (puts[i].edge)
			cg_edge_encode(puts[i].edge, &vals[i]);
		e[i].val = vals[i].data;
		e[i].vlen = vals[i].len;
	}
	rc = prolly_apply(pg->ctx, &pg->root, e, n, &out);
	if (rc == PROLLY_OK)
	{
		pg->root = out;
		if (pg->held)
		{
			cs_ref_root(pg->ctx->cs, &out);
			cs_unref_root(pg->ctx->cs, &old);
		}
	}
	for (size_t i = 0; i < n; i++)
		buf_free(&vals[i]);
	free(vals);
	free(keys);
	free(e);
	return rc;
}

static PHash
pg_root(CommitGraph *g)
{
	return ((ProllyGraph *) g)->root;
}

static int
pg_count(CommitGraph *g, uint64_t *n)
{
	ProllyGraph *pg = (ProllyGraph *) g;

	return prolly_count(pg->ctx, &pg->root, n);
}

static int
pg_foreach(CommitGraph *g, CgEdgeFn fn, void *arg)
{
	ProllyGraph *pg = (ProllyGraph *) g;
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;
	int			stop = 0,
				rc = prolly_iter_open(pg->ctx, &pg->root, NULL, 0, &it);

	if (rc != PROLLY_OK)
		return rc;
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) == 1)
	{
		uint64_t	gen;
		PHash		c;
		CgEdge		e;

		if (!key_decode(k, kl, &gen, &c) || !cg_edge_decode(v, vl, &e))
		{
			rc = PROLLY_ERR_CORRUPT;
			pg->ctx->err_hash = pg->root;
			break;
		}
		if ((stop = fn(arg, gen, &c, &e)) != 0)
			break;
	}
	prolly_iter_close(it);
	return stop ? stop : rc;
}

static void
pg_destroy(CommitGraph *g)
{
	ProllyGraph *pg = (ProllyGraph *) g;

	if (pg->held)
		cs_unref_root(pg->ctx->cs, &pg->root);
	free(pg);
}

static const CommitGraphOps prolly_graph_ops = {
	pg_get, pg_apply, pg_root, pg_count, pg_foreach, pg_destroy
};

CommitGraph *
cgraph_prolly_open(ProllyCtx *ctx, const PHash *root, bool held)
{
	ProllyGraph *pg = px_malloc(sizeof(ProllyGraph));

	memset(pg, 0, sizeof(*pg));
	pg->g.ops = &prolly_graph_ops;
	pg->ctx = ctx;
	pg->held = held;
	if (root)
		pg->root = *root;
	else if (prolly_empty(ctx, 0, &pg->root) != PROLLY_OK)
	{
		free(pg);
		return NULL;
	}
	if (held)
		cs_ref_root(ctx->cs, &pg->root);
	return &pg->g;
}
