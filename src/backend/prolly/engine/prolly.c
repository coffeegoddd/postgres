/*-------------------------------------------------------------------------
 *
 * prolly.c
 *	  Prolly tree nodes, chunking, edits, iteration, diff, merge, chunk
 *	  diff and deltas.  PNode layout is in prolly/engine/chunkfmt.h.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/engine/prolly.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "prolly/engine/prolly.h"
#include "prolly/engine/trace.h"

int			prolly_trace_depth = 0;

static void
trace_reset(XactEvent event, void *arg)
{
	prolly_trace_depth = 0;
}

void
prolly_trace_started(void)
{
	static bool registered = false;

	if (!registered)
	{
		RegisterXactCallback(trace_reset, NULL);
		registered = true;
	}
}

#define REF_VAL_MAX (PHASH_LEN + 10)

const ChunkPolicy prolly_default_policy = {1024, 4096, 16384};

void
prolly_ctx_init(ProllyCtx *ctx, ChunkStore *cs, const ChunkPolicy *policy)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->cs = cs;
	ctx->policy = *policy;
}

void
prolly_ctx_free(ProllyCtx *ctx)
{
}

static int
fail(ProllyCtx *ctx, int err, const PHash *h)
{
	ctx->err = err;
	ctx->err_has_hash = h != NULL;
	if (h)
		ctx->err_hash = *h;
	else
		memset(&ctx->err_hash, 0, sizeof(ctx->err_hash));
	return err;
}

void
prolly_report(ProllyCtx *ctx)
{
	char		hex[PHASH_HEX_LEN + 1];

	if (ctx->err == PROLLY_ERR_BAD_VALUE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a leaf's value holds fewer chunk references than its tree's nodes say")));
	/* (every other error names the chunk at fault) */
	Assert(ctx->err_has_hash);
	phash_hex(&ctx->err_hash, hex);
	if (ctx->err == PROLLY_ERR_MISSING)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s of a tree is not in the store", hex)));
	if (ctx->err == PROLLY_ERR_BAD_DELTA)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s is not a delta of the trees it names", hex)));
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("chunk %s is not a node of the tree read", hex)));
}

int
prolly_put_chunk(ProllyCtx *ctx, const uint8 *data, size_t len, PHash *out)
{
	/*
	 * (new and reused chunks alike held until the transaction ends, by the
	 * store; a reference to one the store hasn't is its error)
	 */
	if (len > PG_UINT32_MAX)
		elog(ERROR, "a chunk of %zu bytes is too long", len);
	(void) cs_put(ctx->cs, data, (uint32) len, out->b);
	return PROLLY_OK;
}

/* ---------------------------------------------------------------- nodes */

typedef struct PNode
{
	PHash		hash;
	uint8	   *bytes;			/* the node's chunk, its own: k and v in it */
	int			level;
	uint8		vrefs;
	ChunkPolicy policy;
	uint32	count;
	const uint8 **k;
	const uint8 **v;
	uint32   *kl;
	uint32   *vl;
	bool		memo;			/* kept by the operation's memo, freed with it */
} PNode;

/*
 * The nodes an operation loads, kept parsed for the rest of it, up to
 * ctx->memo_bytes of memory (a node's chunk, its parsed arrays and its
 * place in the memo): an operation that reads a node again (a merge reads
 * base in both its diffs, and ours' paths once a graft level) finds it
 * here.  Chunks are immutable and the operation holds those it reads, so
 * none kept is ever stale.  A node that would take the memo past its bytes
 * is not kept, so it fills with those loaded first, the upper levels and
 * the paths walked again: the ones read again.
 */
struct NodeMemo
{
	PHashMap	map;			/* a node's hash to 1 + its place in nodes */
	struct PNode **nodes;
	size_t		n,
				cap;
	size_t		used;			/* bytes the nodes kept take */
};

/* What keeping a node of len bytes takes: its chunk, its arrays, its place. */
static size_t
memo_cost(const PNode *n, uint32 len)
{
	return len + sizeof(PNode) +
		n->count * (2 * sizeof(const uint8 *) + 2 * sizeof(uint32)) +
		sizeof(PNode *) + PHASH_LEN + sizeof(uint64);
}

static bool
ref_decode(const uint8 *v, size_t vl, PHash *h, uint64 *total)
{
	const uint8 *p = v + PHASH_LEN;

	if (vl <= PHASH_LEN)
		return false;
	memcpy(h->b, v, PHASH_LEN);
	return read_uvarint(&p, v + vl, total) && p == v + vl;
}

static size_t
ref_encode(uint8 *out, const PHash *h, uint64 total)
{
	size_t		n = PHASH_LEN;

	memcpy(out, h->b, PHASH_LEN);
	while (total >= 0x80)
	{
		out[n++] = (uint8) (total | 0x80);
		total >>= 7;
	}
	out[n++] = (uint8) total;
	return n;
}

static PNode *
node_decode(const PHash *h, const uint8 *data, size_t len)
{
	NodeHeader	hdr;
	const uint8 *p;
	PNode	   *n;
	char	   *mem;

	if (!engine_node_header_parse(data, len, &hdr) || hdr.level >= PROLLY_MAX_HEIGHT)
		return NULL;
	mem = px_malloc(sizeof(PNode) +
					hdr.count * (2 * sizeof(const uint8 *) + 2 * sizeof(uint32)));
	n = (PNode *) mem;
	n->memo = false;
	n->hash = *h;
	n->bytes = NULL;
	n->level = hdr.level;
	n->vrefs = hdr.vrefs;
	n->policy = hdr.policy;
	n->count = hdr.count;
	n->k = (const uint8 **) (mem + sizeof(PNode));
	n->v = n->k + hdr.count;
	n->kl = (uint32 *) (n->v + hdr.count);
	n->vl = n->kl + hdr.count;
	p = hdr.items;
	for (uint32 i = 0; i < n->count; i++)
	{
		uint64	kl,
					vl,
					t;
		PHash		ch;

		if (!read_uvarint(&p, hdr.end, &kl) || kl > (uint64) (hdr.end - p))
			goto bad;
		n->k[i] = p;
		n->kl[i] = (uint32) kl;
		p += kl;
		if (!read_uvarint(&p, hdr.end, &vl) || vl > (uint64) (hdr.end - p))
			goto bad;
		n->v[i] = p;
		n->vl[i] = (uint32) vl;
		p += vl;
		if (n->level > 0 ? !ref_decode(n->v[i], vl, &ch, &t)
			: !engine_leaf_value_valid(n->vrefs, n->v[i], vl))
			goto bad;
		if (i > 0 && key_cmp(n->k[i - 1], n->kl[i - 1], n->k[i], n->kl[i]) >= 0)
			goto bad;
	}
	if (p != hdr.end)
		goto bad;
	return n;
bad:
	px_free(n);
	return NULL;
}

/* Loads a node; on failure records the error and returns NULL. */
static PNode *
node_load(ProllyCtx *ctx, const PHash *h)
{
	uint32		len;
	uint8	   *data;
	PNode	   *n;

	CHECK_FOR_INTERRUPTS();
	if (ctx->memo != NULL)
	{
		uint64	   *at = phmap_find(&ctx->memo->map, h);

		if (at != NULL)
			return ctx->memo->nodes[*at - 1];
	}
	data = cs_get(ctx->cs, h->b, &len);
	if (data == NULL)
	{
		fail(ctx, PROLLY_ERR_MISSING, h);
		return NULL;
	}
	n = node_decode(h, data, len);
	if (n == NULL)
	{
		px_free(data);
		fail(ctx, PROLLY_ERR_CORRUPT, h);
		return NULL;
	}
	n->bytes = data;
	if (ctx->memo != NULL && ctx->memo->used + memo_cost(n, len) <= ctx->memo_bytes)
	{
		NodeMemo   *m = ctx->memo;
		bool		created;

		px_grow((void **) &m->nodes, &m->cap, m->n + 1, sizeof(PNode *));
		m->nodes[m->n++] = n;
		*phmap_ref(&m->map, h, &created) = m->n;
		m->used += memo_cost(n, len);
		n->memo = true;
	}
	return n;
}

static void
node_free(PNode *n)
{
	if (n == NULL || n->memo)
		return;
	px_free(n->bytes);
	px_free(n);
}

static void
node_child(const PNode *n, uint32 i, PHash *h, uint64 *total)
{
	ref_decode(n->v[i], n->vl[i], h, total);	/* validated by node_decode */
}

static uint64
node_total(const PNode *n)
{
	uint64	sum = 0;

	if (n->level == 0)
		return n->count;
	for (uint32 i = 0; i < n->count; i++)
	{
		PHash		h;
		uint64	t;

		node_child(n, i, &h, &t);
		sum += t;
	}
	return sum;
}

/* First index whose key is >= key; count if none. */
static uint32
node_search(const PNode *n, const uint8 *key, size_t klen)
{
	uint32	lo = 0,
				hi = n->count;

	while (lo < hi)
	{
		uint32	mid = lo + (hi - lo) / 2;

		if (key_cmp(n->k[mid], n->kl[mid], key, klen) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* ------------------------------------------------------------- chunking */

typedef struct NodeRef
{
	uint8    *key;			/* last key in the node (owned) */
	uint32	klen;
	PHash		hash;
	uint64	total;
} NodeRef;

typedef struct RefList
{
	NodeRef    *v;
	size_t		n;
	size_t		cap;
} RefList;

static void
reflist_push(RefList *l, const uint8 *key, uint32 klen, const PHash *h,
			 uint64 total)
{
	px_grow((void **) &l->v, &l->cap, l->n + 1, sizeof(NodeRef));
	l->v[l->n].key = px_memdup(key, klen);
	l->v[l->n].klen = klen;
	l->v[l->n].hash = *h;
	l->v[l->n].total = total;
	l->n++;
}

static void
reflist_free(RefList *l)
{
	for (size_t i = 0; i < l->n; i++)
		px_free(l->v[i].key);
	px_free(l->v);
	memset(l, 0, sizeof(*l));
}

static uint64
key_hash64(int level, const uint8 *k, size_t kl)
{
	uint64	h = 0xcbf29ce484222325ULL ^ ((uint64) (level + 1) * 0x9e3779b97f4a7c15ULL);

	for (size_t i = 0; i < kl; i++)
	{
		h ^= k[i];
		h *= 0x100000001b3ULL;
	}
	h ^= h >> 30;
	h *= 0xbf58476d1ce4e5b9ULL;
	h ^= h >> 27;
	h *= 0x94d049bb133111ebULL;
	h ^= h >> 31;
	return h;
}

/*
 * Whether a chunk ends after an item.  Past min_bytes each byte ends the
 * chunk with probability 1 / (target - min), decided by a hash of the key
 * salted with the level, so chunks average target_bytes.  The decision
 * depends only on the item's key and the bytes since the chunk started.
 */
static bool
policy_boundary(const ChunkPolicy *p, int level, const uint8 *k, size_t kl,
				size_t item_bytes, size_t chunk_bytes)
{
	uint64	span,
				thr;

	if (chunk_bytes < p->min_bytes)
		return false;
	if (chunk_bytes >= p->max_bytes)
		return true;
	span = p->target_bytes - p->min_bytes;
	thr = item_bytes >= span ? PG_UINT64_MAX : (PG_UINT64_MAX / span) * item_bytes;
	return key_hash64(level, k, kl) < thr;
}

/* The shape every node of one tree shares. */
typedef struct TreeShape
{
	ChunkPolicy policy;
	uint8		vrefs;
} TreeShape;

/* Packs a stream of items at one level into nodes. */
typedef struct LevelChunker
{
	ProllyCtx  *ctx;
	TreeShape	shape;
	int			level;
	Buf			items;
	uint32	nitems;
	uint64	total;
	size_t		lastkoff;
	uint32	lastklen;
	Buf			scratch;
	RefList    *out;
} LevelChunker;

static void
lc_init(LevelChunker *lc, ProllyCtx *ctx, const TreeShape *shape, int level,
		RefList *out)
{
	memset(lc, 0, sizeof(*lc));
	lc->ctx = ctx;
	lc->shape = *shape;
	lc->level = level;
	lc->out = out;
}

static int
write_node(ProllyCtx *ctx, Buf *scratch, const TreeShape *shape, int level,
		   uint32 nitems, const uint8 *items, size_t ilen, PHash *out)
{
	scratch->len = 0;
	engine_node_header_write(scratch, &shape->policy, level, shape->vrefs, nitems);
	buf_append(scratch, items, ilen);
	return prolly_put_chunk(ctx, scratch->data, scratch->len, out);
}

static int
lc_flush(LevelChunker *lc)
{
	PHash		h;
	int			rc;

	if (lc->nitems == 0)
		return PROLLY_OK;
	rc = write_node(lc->ctx, &lc->scratch, &lc->shape, lc->level, lc->nitems,
					lc->items.data, lc->items.len, &h);
	if (rc != PROLLY_OK)
		return rc;
	reflist_push(lc->out, lc->items.data + lc->lastkoff, lc->lastklen, &h,
				 lc->total);
	lc->items.len = 0;
	lc->nitems = 0;
	lc->total = 0;
	return PROLLY_OK;
}

static int
lc_add(LevelChunker *lc, const uint8 *k, size_t kl, const uint8 *v, size_t vl)
{
	size_t		before = lc->items.len;

	/* (a build, or edits, of many rows reads no node to check at) */
	CHECK_FOR_INTERRUPTS();
	if (lc->level == 0 && !engine_leaf_value_valid(lc->shape.vrefs, v, vl))
		return fail(lc->ctx, PROLLY_ERR_BAD_VALUE, NULL);
	buf_append_uvarint(&lc->items, kl);
	lc->lastkoff = lc->items.len;
	lc->lastklen = (uint32) kl;
	buf_append(&lc->items, k, kl);
	buf_append_uvarint(&lc->items, vl);
	buf_append(&lc->items, v, vl);
	lc->nitems++;
	if (lc->level == 0)
		lc->total++;
	else
	{
		PHash		h;
		uint64	t;

		ref_decode(v, vl, &h, &t);
		lc->total += t;
	}
	if (policy_boundary(&lc->shape.policy, lc->level, k, kl,
						lc->items.len - before, lc->items.len))
		return lc_flush(lc);
	return PROLLY_OK;
}

static void
lc_free(LevelChunker *lc)
{
	buf_free(&lc->items);
	buf_free(&lc->scratch);
}

static int
empty_tree(ProllyCtx *ctx, const TreeShape *shape, PHash *out)
{
	Buf			scratch = BUF_INIT;
	int			rc = write_node(ctx, &scratch, shape, 0, 0, NULL, 0, out);

	buf_free(&scratch);
	return rc;
}

int
prolly_empty(ProllyCtx *ctx, uint8 vrefs, PHash *out)
{
	TreeShape	shape = {ctx->policy, vrefs};

	return empty_tree(ctx, &shape, out);
}

/* Builds levels above refs (nodes at level-1) until one node remains. */
static int
build_up(ProllyCtx *ctx, const TreeShape *shape, RefList *refs, int level,
		 PHash *out)
{
	int			rc = PROLLY_OK;

	while (refs->n > 1 && rc == PROLLY_OK)
	{
		RefList		next = {0};
		LevelChunker lc;
		uint8		val[REF_VAL_MAX];

		lc_init(&lc, ctx, shape, level, &next);
		for (size_t i = 0; i < refs->n && rc == PROLLY_OK; i++)
		{
			size_t		vl = ref_encode(val, &refs->v[i].hash, refs->v[i].total);

			rc = lc_add(&lc, refs->v[i].key, refs->v[i].klen, val, vl);
		}
		if (rc == PROLLY_OK)
			rc = lc_flush(&lc);
		lc_free(&lc);
		reflist_free(refs);
		*refs = next;
		level++;
	}
	if (rc == PROLLY_OK)
	{
		if (refs->n == 0)
			rc = empty_tree(ctx, shape, out);
		else
			*out = refs->v[0].hash;
	}
	reflist_free(refs);
	return rc;
}

int
prolly_build(ProllyCtx *ctx, uint8 vrefs, const ProllyKV *kvs, size_t n,
			 PHash *out)
{
	TreeShape	shape = {ctx->policy, vrefs};
	RefList		refs = {0};
	LevelChunker lc;
	int			rc = PROLLY_OK;

	lc_init(&lc, ctx, &shape, 0, &refs);
	for (size_t i = 0; i < n && rc == PROLLY_OK; i++)
		rc = lc_add(&lc, kvs[i].key, kvs[i].klen, kvs[i].val, kvs[i].vlen);
	if (rc == PROLLY_OK)
		rc = lc_flush(&lc);
	lc_free(&lc);
	if (rc != PROLLY_OK)
	{
		reflist_free(&refs);
		return rc;
	}
	return build_up(ctx, &shape, &refs, 1, out);
}

/* --------------------------------------------------------------- cursor */

/*
 * A cursor holds the path of loaded nodes from the root down to level d and
 * points at entry i[d] of n[d].  Above the leaves that entry is a child that
 * has not been loaded yet, which lets diff skip equal subtrees unread.  If
 * loaded is set, every node the cursor loads is recorded there.
 */
typedef struct Cursor
{
	ProllyCtx  *ctx;
	int			top;
	int			d;
	bool		end;
	PHashMap   *loaded;
	PNode	   *n[PROLLY_MAX_HEIGHT];
	uint32	i[PROLLY_MAX_HEIGHT];
} Cursor;

static void
note_loaded(Cursor *c, const PHash *h)
{
	bool		created;

	if (c->loaded)
		phmap_ref(c->loaded, h, &created);
}

static int
cursor_open(Cursor *c, ProllyCtx *ctx, const PHash *root, PHashMap *loaded)
{
	PNode	   *r;

	memset(c, 0, sizeof(*c));
	c->ctx = ctx;
	c->loaded = loaded;
	c->end = true;
	r = node_load(ctx, root);
	if (r == NULL)
		return ctx->err;
	note_loaded(c, root);
	c->top = c->d = r->level;
	c->n[c->top] = r;
	c->end = r->count == 0;
	return PROLLY_OK;
}

static void
cursor_close(Cursor *c)
{
	for (int l = 0; l < PROLLY_MAX_HEIGHT; l++)
		if (c->n[l])
			node_free(c->n[l]);
	memset(c->n, 0, sizeof(c->n));
}

static int
cursor_descend(Cursor *c)
{
	PHash		h;
	uint64	t;
	PNode	   *n;
	PNode	   *parent = c->n[c->d];

	node_child(parent, c->i[c->d], &h, &t);
	n = node_load(c->ctx, &h);
	if (n == NULL)
		return c->ctx->err;
	/* a child must sit exactly one level down and match its parent's shape */
	if (n->level != c->d - 1 || n->count == 0 || n->vrefs != parent->vrefs ||
		memcmp(&n->policy, &parent->policy, sizeof(ChunkPolicy)) != 0)
	{
		node_free(n);
		return fail(c->ctx, PROLLY_ERR_CORRUPT, &h);
	}
	note_loaded(c, &h);
	c->d--;
	if (c->n[c->d])
		node_free(c->n[c->d]);
	c->n[c->d] = n;
	c->i[c->d] = 0;
	return PROLLY_OK;
}

/* Moves to the next entry at level d, popping up past finished nodes. */
static void
cursor_advance(Cursor *c)
{
	c->i[c->d]++;
	while (c->i[c->d] >= c->n[c->d]->count)
	{
		if (c->d == c->top)
		{
			c->end = true;
			return;
		}
		node_free(c->n[c->d]);
		c->n[c->d] = NULL;
		c->d++;
		c->i[c->d]++;
	}
}

static int
cursor_down_to(Cursor *c, int level)
{
	while (!c->end && c->d > level)
	{
		int			rc = cursor_descend(c);

		if (rc != PROLLY_OK)
			return rc;
	}
	return PROLLY_OK;
}

static int
cursor_next_item(Cursor *c)
{
	cursor_advance(c);
	return cursor_down_to(c, 0);
}

/* Positions the cursor in the node at `level` that would hold key. */
static int
cursor_seek(Cursor *c, ProllyCtx *ctx, const PHash *root, int level,
			const uint8 *key, size_t klen)
{
	int			rc = cursor_open(c, ctx, root, NULL);

	if (rc != PROLLY_OK || c->end)
		return rc;
	while (c->d > level)
	{
		PNode	   *n = c->n[c->d];
		uint32	i = key ? node_search(n, key, klen) : 0;

		c->i[c->d] = i < n->count ? i : n->count - 1;
		rc = cursor_descend(c);
		if (rc != PROLLY_OK)
			return rc;
	}
	c->i[level] = key ? node_search(c->n[level], key, klen) : 0;
	return PROLLY_OK;
}

static bool
cursor_is_last_node(const Cursor *c, int level)
{
	for (int l = level + 1; l <= c->top; l++)
		if (c->i[l] + 1 < c->n[l]->count)
			return false;
	return true;
}

static int
cursor_next_node(Cursor *c, int level)
{
	c->i[level] = c->n[level]->count - 1;
	cursor_advance(c);
	return cursor_down_to(c, level);
}

/* ---------------------------------------------------------------- reads */

int
prolly_get(ProllyCtx *ctx, const PHash *root, const uint8 *key, size_t klen,
		   const uint8 **val, size_t *vlen)
{
	PNode	   *n = node_load(ctx, root);
	uint32	i;
	int			rc;

	if (n == NULL)
		return ctx->err;
	while (n->level > 0)
	{
		PHash		h;
		uint64	t;
		int			want = n->level - 1;

		i = node_search(n, key, klen);
		if (i >= n->count)
		{
			node_free(n);
			return PROLLY_NOT_FOUND;
		}
		node_child(n, i, &h, &t);
		node_free(n);
		n = node_load(ctx, &h);
		if (n == NULL)
			return ctx->err;
		if (n->level != want)
		{
			node_free(n);
			return fail(ctx, PROLLY_ERR_CORRUPT, &h);
		}
	}
	i = node_search(n, key, klen);
	rc = PROLLY_NOT_FOUND;
	if (i < n->count && bytes_eq(n->k[i], n->kl[i], key, klen))
	{
		/* (a copy, the caller's: the node, and its bytes, freed now) */
		*val = px_memdup(n->v[i], n->vl[i]);
		*vlen = n->vl[i];
		rc = PROLLY_OK;
	}
	node_free(n);
	return rc;
}

int
prolly_count(ProllyCtx *ctx, const PHash *root, uint64 *out)
{
	PNode	   *n = node_load(ctx, root);

	if (n == NULL)
		return ctx->err;
	*out = node_total(n);
	node_free(n);
	return PROLLY_OK;
}

int
prolly_height(ProllyCtx *ctx, const PHash *root, int *out)
{
	PNode	   *n = node_load(ctx, root);

	if (n == NULL)
		return ctx->err;
	*out = n->level + 1;
	node_free(n);
	return PROLLY_OK;
}

struct ProllyIter
{
	Cursor		c;
	int			err;
	MemoryContext cxt;			/* opened in: its nodes are read into it */
	Buf			key;			/* the item returned last, its own copy: the */
	Buf			val;			/* cursor frees a leaf as it leaves it */
};

int
prolly_iter_open(ProllyCtx *ctx, const PHash *root, const uint8 *start,
				 size_t slen, ProllyIter **out)
{
	ProllyIter *it = px_malloc(sizeof(ProllyIter));
	int			rc;

	it->key = (Buf) BUF_INIT;
	it->val = (Buf) BUF_INIT;
	it->cxt = CurrentMemoryContext;
	rc = cursor_seek(&it->c, ctx, root, 0, start, slen);

	if (rc == PROLLY_OK && !it->c.end && it->c.i[0] >= it->c.n[0]->count)
	{
		it->c.i[0] = it->c.n[0]->count - 1;
		rc = cursor_next_item(&it->c);
	}
	if (rc != PROLLY_OK)
	{
		cursor_close(&it->c);
		px_free(it);
		return rc;
	}
	it->err = PROLLY_OK;
	*out = it;
	return PROLLY_OK;
}

int
prolly_iter_next(ProllyIter *it, const uint8 **key, size_t *klen,
				 const uint8 **val, size_t *vlen)
{
	Cursor	   *c = &it->c;
	PNode	   *n;
	uint32		i;
	MemoryContext caller;

	if (it->err != PROLLY_OK)
		return it->err;
	if (c->end)
		return 0;

	/*
	 * (the nodes the cursor reads, and the item's copy, in the context the
	 * iterator was opened in: a caller's context reset between calls would
	 * free a node the cursor holds)
	 */
	caller = MemoryContextSwitchTo(it->cxt);
	n = c->n[0];
	i = c->i[0];

	/*
	 * (copied: the cursor's next step may free the leaf; valid until the
	 * next call, or the close)
	 */
	it->key.len = 0;
	buf_append(&it->key, n->k[i], n->kl[i]);
	it->val.len = 0;
	buf_append(&it->val, n->v[i], n->vl[i]);
	*key = it->key.data;
	*klen = n->kl[i];
	*val = it->val.data;
	*vlen = n->vl[i];
	it->err = cursor_next_item(c);
	MemoryContextSwitchTo(caller);
	return 1;
}

void
prolly_iter_close(ProllyIter *it)
{
	MemoryContext caller = MemoryContextSwitchTo(it->cxt);

	cursor_close(&it->c);
	buf_free(&it->key);
	buf_free(&it->val);
	MemoryContextSwitchTo(caller);
	px_free(it);
}

/* ---------------------------------------------------------------- edits */

typedef struct EditEnt
{
	size_t		koff,
				klen,
				voff,
				vlen;
	bool		del;
} EditEnt;

/* Edits for the next level up, with keys and values in an owned arena. */
typedef struct EditBuf
{
	Buf			arena;
	EditEnt    *v;
	size_t		n;
	size_t		cap;
} EditBuf;

static void
editbuf_add(EditBuf *eb, const uint8 *k, size_t kl, const uint8 *v,
			size_t vl, bool del)
{
	EditEnt    *e;

	px_grow((void **) &eb->v, &eb->cap, eb->n + 1, sizeof(EditEnt));
	e = &eb->v[eb->n++];
	e->koff = eb->arena.len;
	e->klen = kl;
	buf_append(&eb->arena, k, kl);
	e->voff = eb->arena.len;
	e->vlen = vl;
	buf_append(&eb->arena, v, vl);
	e->del = del;
}

static ProllyEdit *
editbuf_edits(const EditBuf *eb)
{
	ProllyEdit *out = px_malloc(eb->n * sizeof(ProllyEdit));

	for (size_t i = 0; i < eb->n; i++)
	{
		out[i].key = eb->arena.data + eb->v[i].koff;
		out[i].klen = eb->v[i].klen;
		out[i].val = eb->arena.data + eb->v[i].voff;
		out[i].vlen = eb->v[i].vlen;
		out[i].del = eb->v[i].del;
	}
	return out;
}

static void
editbuf_free(EditBuf *eb)
{
	buf_free(&eb->arena);
	px_free(eb->v);
	memset(eb, 0, sizeof(*eb));
}

typedef struct OldRef
{
	uint8	   *key;			/* its own copy: the node is freed as the
								 * cursor leaves it */
	uint32	klen;
	PHash		hash;
} OldRef;

/* Old nodes replaced and new nodes written for one run of edits. */
static void
emit_parent_edits(EditBuf *parent, const OldRef *old, size_t nold,
				  const RefList *fresh)
{
	size_t		a = 0,
				b = 0;
	uint8		val[REF_VAL_MAX];

	while (a < nold || b < fresh->n)
	{
		int			cmp = a >= nold ? 1 : b >= fresh->n ? -1 :
			key_cmp(old[a].key, old[a].klen, fresh->v[b].key, fresh->v[b].klen);

		if (cmp < 0)
		{
			editbuf_add(parent, old[a].key, old[a].klen, NULL, 0, true);
			a++;
		}
		else
		{
			const NodeRef *r = &fresh->v[b];

			if (cmp > 0 || !phash_eq(&old[a].hash, &r->hash))
				editbuf_add(parent, r->key, r->klen, val,
							ref_encode(val, &r->hash, r->total), false);
			if (cmp == 0)
				a++;
			b++;
		}
	}
}

/*
 * Applies sorted, unique edits to the items at one level of the tree at
 * root.  For each run of edits, rechunks from the start of the first node
 * the run touches and keeps going until a new boundary lands on an old one;
 * from there on the old nodes are reused as they are.  The replaced nodes
 * become edits to the level above (written to parent), or, at the root
 * level, the new nodes are returned in top_refs.
 */
static int
level_apply(ProllyCtx *ctx, const PHash *root, const TreeShape *shape,
			int level, const ProllyEdit *ed, size_t ne, EditBuf *parent,
			RefList *top_refs)
{
	size_t		e = 0;
	OldRef	   *old = NULL;
	size_t		oldcap = 0;
	int			rc = PROLLY_OK;

	while (e < ne && rc == PROLLY_OK)
	{
		Cursor		c;
		RefList		fresh = {0};
		LevelChunker lc;
		size_t		nold = 0;

		rc = cursor_seek(&c, ctx, root, level, ed[e].key, ed[e].klen);
		if (rc != PROLLY_OK)
		{
			cursor_close(&c);
			break;
		}
		lc_init(&lc, ctx, shape, level, &fresh);
		for (;;)
		{
			PNode	   *nd = c.n[level];
			bool		last = cursor_is_last_node(&c, level);
			const uint8 *lk = nd->count ? nd->k[nd->count - 1] : NULL;
			uint32	lkl = nd->count ? nd->kl[nd->count - 1] : 0;
			uint32	j = 0;

			while (rc == PROLLY_OK)
			{
				bool		have_item = j < nd->count;
				bool		have_edit = e < ne &&
					(last || key_cmp(ed[e].key, ed[e].klen, lk, lkl) <= 0);
				int			cmp;

				if (!have_item && !have_edit)
					break;
				cmp = !have_item ? 1 : !have_edit ? -1 :
					key_cmp(nd->k[j], nd->kl[j], ed[e].key, ed[e].klen);
				if (cmp < 0)
				{
					rc = lc_add(&lc, nd->k[j], nd->kl[j], nd->v[j], nd->vl[j]);
					j++;
				}
				else
				{
					if (!ed[e].del)
						rc = lc_add(&lc, ed[e].key, ed[e].klen, ed[e].val, ed[e].vlen);
					if (cmp == 0)
						j++;
					e++;
				}
			}
			if (rc != PROLLY_OK)
				break;
			if (nd->count)
			{
				px_grow((void **) &old, &oldcap, nold + 1, sizeof(OldRef));
				old[nold].key = px_memdup(lk, lkl);
				old[nold].klen = lkl;
				old[nold].hash = nd->hash;
				nold++;
			}
			if (last)
			{
				rc = lc_flush(&lc);
				break;
			}
			if (lc.nitems == 0)
				break;			/* resynchronized with the old boundaries */
			rc = cursor_next_node(&c, level);
			if (rc != PROLLY_OK)
				break;
			if (c.end)
			{
				rc = fail(ctx, PROLLY_ERR_CORRUPT, root);
				break;
			}
		}
		lc_free(&lc);
		if (rc == PROLLY_OK)
		{
			if (top_refs)
				for (size_t i = 0; i < fresh.n; i++)
					reflist_push(top_refs, fresh.v[i].key, fresh.v[i].klen,
								 &fresh.v[i].hash, fresh.v[i].total);
			else
				emit_parent_edits(parent, old, nold, &fresh);
		}
		for (size_t i = 0; i < nold; i++)
			px_free(old[i].key);
		cursor_close(&c);
		reflist_free(&fresh);
	}
	px_free(old);
	return rc;
}

typedef struct SortEdit
{
	ProllyEdit	e;
	size_t		seq;
} SortEdit;

static int
sortedit_cmp(const void *pa, const void *pb)
{
	const SortEdit *a = pa;
	const SortEdit *b = pb;
	int			c = key_cmp(a->e.key, a->e.klen, b->e.key, b->e.klen);

	if (c != 0)
		return c;
	return (a->seq > b->seq) - (a->seq < b->seq);
}

/* A root with one child is not canonical: the child is the root. */
static int
collapse(ProllyCtx *ctx, PHash root, PHash *out)
{
	for (;;)
	{
		PNode	   *n = node_load(ctx, &root);
		uint64	t;

		if (n == NULL)
			return ctx->err;
		if (n->level == 0 || n->count != 1)
		{
			node_free(n);
			*out = root;
			return PROLLY_OK;
		}
		node_child(n, 0, &root, &t);
		node_free(n);
	}
}

/*
 * A graft: an entry of a node at 'level' (a child's last key and its ref)
 * to put in place of the same key's entry in another tree, the child
 * covering the keys (lo, key], from the start of the tree if !has_lo.
 */
typedef struct Graft
{
	int			level;
	uint8    *key;
	uint32	klen;
	uint8    *lo;
	uint32	lolen;
	bool		has_lo;
	uint8    *val;
	uint32	vlen;
} Graft;

typedef struct GraftList
{
	Graft	   *v;
	size_t		n;
	size_t		cap;
} GraftList;

static void
graftlist_free(GraftList *g)
{
	for (size_t i = 0; i < g->n; i++)
	{
		px_free(g->v[i].key);
		px_free(g->v[i].lo);
		px_free(g->v[i].val);
	}
	px_free(g->v);
	memset(g, 0, sizeof(*g));
}

/* The edits at one level, merged in key order with the grafts on that level. */
static ProllyEdit *
with_grafts(ProllyEdit *cur, size_t *ncur, const GraftList *g, int level)
{
	ProllyEdit *out;
	size_t		n = 0,
				a = 0;

	for (size_t k = 0; k < g->n; k++)
		n += g->v[k].level == level;
	if (n == 0)
		return cur;
	out = px_malloc((*ncur + n) * sizeof(ProllyEdit));
	n = 0;
	for (size_t k = 0; k < g->n; k++)
	{
		const Graft *gr = &g->v[k];

		if (gr->level != level)
			continue;
		while (a < *ncur && key_cmp(cur[a].key, cur[a].klen, gr->key, gr->klen) < 0)
			out[n++] = cur[a++];
		out[n].key = gr->key;
		out[n].klen = gr->klen;
		out[n].val = gr->val;
		out[n].vlen = gr->vlen;
		out[n].del = false;
		n++;
	}
	while (a < *ncur)
		out[n++] = cur[a++];
	px_free(cur);
	*ncur = n;
	return out;
}

static int	apply_core(ProllyCtx *ctx, const PHash *root, const ProllyEdit *edits,
					   size_t n, const GraftList *grafts, int glevel, PHash *out);

int
prolly_apply(ProllyCtx *ctx, const PHash *root, const ProllyEdit *edits,
			 size_t n, PHash *out)
{
	return apply_core(ctx, root, edits, n, NULL, 0, out);
}

/*
 * Applies edits to the items of the tree at root, and, given grafts, puts
 * the entries of those on level glevel in place, as edits to that level.
 */
static int
apply_core(ProllyCtx *ctx, const PHash *root, const ProllyEdit *edits,
		   size_t n, const GraftList *grafts, int glevel, PHash *out)
{
	int			gtop = grafts ? glevel : -1;
	SortEdit   *s = px_malloc(n * sizeof(SortEdit));
	ProllyEdit *cur = px_malloc(n * sizeof(ProllyEdit));
	size_t		ncur = 0;
	EditBuf		eb = {0};
	PNode	   *r;
	TreeShape	shape;
	int			top,
				rc = PROLLY_OK;

	r = node_load(ctx, root);
	if (r == NULL)
	{
		px_free(s);
		px_free(cur);
		return ctx->err;
	}
	/* edits continue with the tree's own policy and value layout */
	shape.policy = r->policy;
	shape.vrefs = r->vrefs;
	top = r->level;
	node_free(r);

	for (size_t i = 0; i < n; i++)
	{
		s[i].e = edits[i];
		s[i].seq = i;
	}
	if (n > 1)
		qsort(s, n, sizeof(SortEdit), sortedit_cmp);
	for (size_t i = 0; i < n; i++)
	{
		if (i + 1 < n && key_cmp(s[i].e.key, s[i].e.klen,
								 s[i + 1].e.key, s[i + 1].e.klen) == 0)
			continue;			/* a later edit of the same key wins */
		cur[ncur++] = s[i].e;
	}
	px_free(s);

	*out = *root;
	for (int level = 0; (ncur > 0 || level <= gtop) && rc == PROLLY_OK; level++)
	{
		if (grafts && level == glevel)
			cur = with_grafts(cur, &ncur, grafts, level);
		if (level < top)
		{
			EditBuf		next = {0};

			rc = level_apply(ctx, root, &shape, level, cur, ncur, &next, NULL);
			px_free(cur);
			editbuf_free(&eb);
			eb = next;
			cur = editbuf_edits(&eb);
			ncur = eb.n;
		}
		else
		{
			RefList		refs = {0};

			rc = level_apply(ctx, root, &shape, level, cur, ncur, NULL, &refs);
			if (rc != PROLLY_OK)
				reflist_free(&refs);
			else if (refs.n == 0)
			{
				rc = empty_tree(ctx, &shape, out);
				reflist_free(&refs);
			}
			else if (refs.n == 1)
			{
				rc = collapse(ctx, refs.v[0].hash, out);
				reflist_free(&refs);
			}
			else
				rc = build_up(ctx, &shape, &refs, level + 1, out);
			break;
		}
	}
	px_free(cur);
	editbuf_free(&eb);
	if (rc != PROLLY_OK)
		*out = *root;
	return rc;
}

/* ----------------------------------------------------------------- diff */

static void
emit_item(Cursor *c, bool removed, ProllyDiffFn fn, void *arg)
{
	PNode	   *n = c->n[0];
	uint32	i = c->i[0];
	ProllyDiff	d = {n->k[i], n->kl[i], NULL, 0, NULL, 0};

	if (fn == NULL)
		return;
	if (removed)
	{
		d.from = n->v[i];
		d.flen = n->vl[i];
	}
	else
	{
		d.to = n->v[i];
		d.tlen = n->vl[i];
	}
	fn(arg, &d);
}

static int
drain(Cursor *c, bool removed, ProllyDiffFn fn, void *arg)
{
	while (!c->end)
	{
		if (c->d > 0)
		{
			int			rc = cursor_descend(c);

			if (rc != PROLLY_OK)
				return rc;
			continue;
		}
		emit_item(c, removed, fn, arg);
		cursor_advance(c);
	}
	return PROLLY_OK;
}

/*
 * The key just before the entry the cursor is at on level d: the entry
 * before it in its node, or, for a node's first entry, the key of the
 * entry before its parent's (the last key of the subtree before).  False
 * at the start of the tree.
 */
static bool
entry_prev_key(const Cursor *c, int d, const uint8 **k, uint32 *kl)
{
	for (int l = d; l <= c->top; l++)
		if (c->n[l] && c->i[l] > 0)
		{
			*k = c->n[l]->k[c->i[l] - 1];
			*kl = c->n[l]->kl[c->i[l] - 1];
			return true;
		}
	return false;
}

static bool
same_lo(bool ha, const uint8 *a, uint32 al, bool hb, const uint8 *b, uint32 bl)
{
	return ha == hb && (!ha || key_cmp(a, al, b, bl) == 0);
}

/*
 * Whether the tree at root has, on level 'level', the entry (key -> the
 * child h), right after the key lo: the same subtree, covering the same
 * keys.  Reads root's path down to that level.
 */
static int
tree_has_entry(ProllyCtx *ctx, const PHash *root, int level,
			   const uint8 *key, uint32 klen, bool has_lo,
			   const uint8 *lo, uint32 lolen, const PHash *h, bool *has)
{
	Cursor		c;
	int			rc;

	*has = false;
	rc = cursor_seek(&c, ctx, root, level, key, klen);
	if (rc == PROLLY_OK && !c.end && c.top >= level && c.n[level] &&
		c.i[level] < c.n[level]->count)
	{
		PNode	   *n = c.n[level];
		uint32	i = c.i[level];
		PHash		ch;
		uint64	t;
		const uint8 *pk = NULL;
		uint32	pkl = 0;
		bool		hp;

		if (key_cmp(n->k[i], n->kl[i], key, klen) == 0)
		{
			node_child(n, i, &ch, &t);
			hp = entry_prev_key(&c, level, &pk, &pkl);
			*has = phash_eq(&ch, h) && same_lo(hp, pk, pkl, has_lo, lo, lolen);
		}
	}
	cursor_close(&c);
	return rc;
}

/*
 * Walks two trees in key order, skipping subtrees with equal hashes
 * without reading them.  Every node either side does read is recorded in
 * la / lb when given.
 *
 * With 'other' (a third tree changed from 'from' on its own), a subtree
 * 'to' changed where 'other' still has from's subtree, covering the same
 * keys, is not walked either: none of other's changes are in it.  Such a
 * subtree of 'to' goes to grafts, if given, to be taken whole.
 */
static int
diff_core(ProllyCtx *ctx, const PHash *from, const PHash *to,
		  ProllyDiffFn fn, void *arg, PHashMap *la, PHashMap *lb,
		  const PHash *other, GraftList *grafts)
{
	Cursor		a,
				b;
	int			rc;

	rc = cursor_open(&a, ctx, from, la);
	if (rc == PROLLY_OK)
		rc = cursor_open(&b, ctx, to, lb);
	else
		memset(&b, 0, sizeof(b));
	while (rc == PROLLY_OK && !a.end && !b.end)
	{
		int			da = a.d,
					db = b.d;

		if (da > 0 || db > 0)
		{
			if (da == db)
			{
				PHash		ha,
							hb;
				uint64	t;

				node_child(a.n[da], a.i[da], &ha, &t);
				node_child(b.n[db], b.i[db], &hb, &t);
				if (phash_eq(&ha, &hb))
				{
					/* identical subtrees: skip both without reading them */
					cursor_advance(&a);
					cursor_advance(&b);
					continue;
				}
				if (other)
				{
					PNode	   *na = a.n[da],
							   *nb = b.n[db];
					uint32	ia = a.i[da],
								ib = b.i[db];
					const uint8 *la_k = NULL,
							   *lb_k = NULL;
					uint32	la_l = 0,
								lb_l = 0;
					bool		hla = entry_prev_key(&a, da, &la_k, &la_l),
								hlb = entry_prev_key(&b, db, &lb_k, &lb_l),
								has = false;

					/* the same keys on both sides, and other has from's */
					if (key_cmp(na->k[ia], na->kl[ia], nb->k[ib], nb->kl[ib]) == 0 &&
						same_lo(hla, la_k, la_l, hlb, lb_k, lb_l))
						rc = tree_has_entry(ctx, other, da, na->k[ia], na->kl[ia],
											hla, la_k, la_l, &ha, &has);
					if (rc != PROLLY_OK)
						continue;
					if (has)
					{
						if (grafts)
						{
							Graft	   *g;

							px_grow((void **) &grafts->v, &grafts->cap,
									grafts->n + 1, sizeof(Graft));
							g = &grafts->v[grafts->n++];
							g->level = db;
							g->key = px_memdup(nb->k[ib], nb->kl[ib]);
							g->klen = nb->kl[ib];
							g->has_lo = hlb;
							g->lo = hlb ? px_memdup(lb_k, lb_l) : NULL;
							g->lolen = lb_l;
							g->val = px_memdup(nb->v[ib], nb->vl[ib]);
							g->vlen = nb->vl[ib];
						}
						cursor_advance(&a);
						cursor_advance(&b);
						continue;
					}
				}
			}
			if (da >= db && da > 0)
				rc = cursor_descend(&a);
			if (rc == PROLLY_OK && db >= da && db > 0)
				rc = cursor_descend(&b);
			continue;
		}
		else
		{
			PNode	   *na = a.n[0],
					   *nb = b.n[0];
			uint32	ia = a.i[0],
						ib = b.i[0];
			int			cmp = key_cmp(na->k[ia], na->kl[ia], nb->k[ib], nb->kl[ib]);

			if (cmp < 0)
			{
				emit_item(&a, true, fn, arg);
				cursor_advance(&a);
			}
			else if (cmp > 0)
			{
				emit_item(&b, false, fn, arg);
				cursor_advance(&b);
			}
			else
			{
				if (fn && !bytes_eq(na->v[ia], na->vl[ia], nb->v[ib], nb->vl[ib]))
				{
					ProllyDiff	d = {na->k[ia], na->kl[ia], na->v[ia], na->vl[ia],
					nb->v[ib], nb->vl[ib]};

					fn(arg, &d);
				}
				cursor_advance(&a);
				cursor_advance(&b);
			}
		}
	}
	if (rc == PROLLY_OK)
		rc = drain(&a, true, fn, arg);
	if (rc == PROLLY_OK)
		rc = drain(&b, false, fn, arg);
	cursor_close(&a);
	cursor_close(&b);
	return rc;
}

int
prolly_diff(ProllyCtx *ctx, const PHash *from, const PHash *to,
			ProllyDiffFn fn, void *arg)
{
	if (phash_eq(from, to))
		return PROLLY_OK;
	return diff_core(ctx, from, to, fn, arg, NULL, NULL, NULL, NULL);
}

static void
collect_fn(void *arg, const ProllyDiff *d)
{
	ProllyDiffList *l = arg;
	ProllyDiff *c;

	px_grow((void **) &l->v, &l->cap, l->n + 1, sizeof(ProllyDiff));
	c = &l->v[l->n++];

	/*
	 * (the bytes copied, the caller's: the nodes they are in are freed as
	 * the diff goes on)
	 */
	*c = *d;
	c->key = px_memdup(d->key, d->klen);
	c->from = d->from ? px_memdup(d->from, d->flen) : NULL;
	c->to = d->to ? px_memdup(d->to, d->tlen) : NULL;
}

int
prolly_diff_collect(ProllyCtx *ctx, const PHash *from, const PHash *to,
					ProllyDiffList *out)
{
	memset(out, 0, sizeof(*out));
	return prolly_diff(ctx, from, to, collect_fn, out);
}

void
prolly_diff_list_free(ProllyDiffList *l)
{
	for (size_t i = 0; i < l->n; i++)
	{
		px_free((void *) l->v[i].key);
		px_free((void *) l->v[i].from);
		px_free((void *) l->v[i].to);
	}
	px_free(l->v);
	memset(l, 0, sizeof(*l));
}

/*
 * A node read on a's side that is also a node of b was read on b's side
 * too: b could only have skipped it inside a subtree equal to one a also
 * skipped, and a node occurs once in a tree.  So the nodes of a not in b
 * are exactly a's reads minus b's.
 */
int
prolly_chunk_diff(ProllyCtx *ctx, const PHash *a, const PHash *b,
				  ProllyChunkFn fn, void *arg)
{
	PHashMap	la,
				lb;
	int			rc;

	if (phash_eq(a, b))
		return PROLLY_OK;
	phmap_init(&la);
	phmap_init(&lb);
	rc = diff_core(ctx, a, b, NULL, NULL, &la, &lb, NULL, NULL);
	if (rc == PROLLY_OK)
		for (size_t i = 0; i < la.cap; i++)
			if (la.used[i] && !phmap_find(&lb, &la.keys[i]))
				fn(arg, &la.keys[i]);
	phmap_free(&la);
	phmap_free(&lb);
	return rc;
}

typedef struct ValueTrees
{
	ProllyCtx  *ctx;
	ProllyChunkFn fn;
	void	   *arg;
	bool		from_counted;
	PHash		empty;
	PHashMap	seen;
	int			rc;
} ValueTrees;

static void
value_tree_node(void *arg, const PHash *h)
{
	ValueTrees *w = arg;
	bool		created;

	(void) phmap_ref(&w->seen, h, &created);
	if (created)
		w->fn(w->arg, h);
}

static void
value_trees_of_change(void *arg, const ProllyDiff *d)
{
	ValueTrees *w = arg;
	const uint8 *to,
			   *from = NULL;
	uint64		nto,
				nfrom = 0;

	if (d->to == NULL || w->rc != PROLLY_OK)
		return;
	if (!engine_leaf_value_refs(d->to, d->tlen, &to, &nto) ||
		(d->from != NULL && w->from_counted &&
		 !engine_leaf_value_refs(d->from, d->flen, &from, &nfrom)))
	{
		w->rc = fail(w->ctx, PROLLY_ERR_BAD_VALUE, NULL);
		return;
	}
	for (uint64 i = 0; i < nto && w->rc == PROLLY_OK; i++)
	{
		PHash		tree,
					old = w->empty;

		memcpy(tree.b, to + i * PHASH_LEN, PHASH_LEN);
		if (i < nfrom)
			memcpy(old.b, from + i * PHASH_LEN, PHASH_LEN);
		w->rc = prolly_chunk_diff(w->ctx, &tree, &old, value_tree_node, w);
	}
}

static int
vrefs_of(ProllyCtx *ctx, const PHash *root, uint8 *vrefs)
{
	PNode	   *n = node_load(ctx, root);

	if (n == NULL)
		return ctx->err;
	*vrefs = n->vrefs;
	node_free(n);
	return PROLLY_OK;
}

int
prolly_chunk_diff_values(ProllyCtx *ctx, const PHash *a, const PHash *b,
						 ProllyChunkFn fn, void *arg)
{
	ValueTrees	w = {ctx, fn, arg};
	uint8		va = 0,
				vb = 0;
	int			rc;

	if (phash_eq(a, b))
		return PROLLY_OK;
	rc = vrefs_of(ctx, a, &va);
	if (rc == PROLLY_OK)
		rc = vrefs_of(ctx, b, &vb);
	if (rc != PROLLY_OK || va != CHUNK_VREFS_COUNTED)
		return rc;
	w.from_counted = vb == CHUNK_VREFS_COUNTED;
	rc = prolly_empty(ctx, 0, &w.empty);
	if (rc != PROLLY_OK)
		return rc;
	phmap_init(&w.seen);
	rc = prolly_diff(ctx, b, a, value_trees_of_change, &w);
	phmap_free(&w.seen);
	return rc != PROLLY_OK ? rc : w.rc;
}

/* ---------------------------------------------------------------- merge */

static bool
same_change(const ProllyDiff *a, const ProllyDiff *b)
{
	if (a->to == NULL || b->to == NULL)
		return a->to == NULL && b->to == NULL;
	return bytes_eq(a->to, a->tlen, b->to, b->tlen);
}

/*
 * The three-way merge.  A subtree only one side changed is not walked by
 * either diff: one of ours stays, one of theirs goes in whole, by its ref
 * (a graft).  The grafts go in first, a level at a time from the highest,
 * then theirs' row edits: each step an apply to the tree as the steps
 * before left it, so every rechunking reads the merged rows.
 */
static int
merge_core(ProllyCtx *ctx, const PHash *base, const PHash *ours,
		   const PHash *theirs, ProllyMergeResult *out)
{
	ProllyDiffList dours = {0},
				dtheirs = {0};
	GraftList	grafts = {0};
	ProllyEdit *edits;
	size_t		nedits = 0,
				conflict_cap = 0,
				a = 0,
				b = 0;
	int			rc,
				gtop = 0;
	PHash		root;
	TraceSpan	td1,
				td2,
				ta;

	memset(out, 0, sizeof(*out));
	out->root = *ours;
	if (phash_eq(ours, theirs) || phash_eq(base, theirs))
		return PROLLY_OK;
	if (phash_eq(base, ours))
	{
		out->root = *theirs;
		return PROLLY_OK;
	}
	td1 = trace_begin("merge: diff base-ours");

	rc = diff_core(ctx, base, ours, collect_fn, &dours, NULL, NULL, theirs, NULL);
	trace_end(td1, "%zu changes", dours.n);
	if (rc != PROLLY_OK)
	{
		prolly_diff_list_free(&dours);
		return rc;
	}
	td2 = trace_begin("merge: diff base-theirs");

	rc = diff_core(ctx, base, theirs, collect_fn, &dtheirs, NULL, NULL, ours, &grafts);
	trace_end(td2, "%zu changes, %zu grafts", dtheirs.n, grafts.n);
	if (rc != PROLLY_OK)
	{
		prolly_diff_list_free(&dours);
		prolly_diff_list_free(&dtheirs);
		graftlist_free(&grafts);
		return rc;
	}
	edits = px_malloc((dtheirs.n ? dtheirs.n : 1) * sizeof(ProllyEdit));
	while (b < dtheirs.n)
	{
		const ProllyDiff *t = &dtheirs.v[b];
		int			cmp = a >= dours.n ? 1 :
			key_cmp(dours.v[a].key, dours.v[a].klen, t->key, t->klen);

		if (cmp < 0)
		{
			a++;				/* only ours changed this key */
			continue;
		}
		if (cmp > 0)
		{
			ProllyEdit *e = &edits[nedits++];

			e->key = t->key;
			e->klen = t->klen;
			e->val = t->to;
			e->vlen = t->tlen;
			e->del = t->to == NULL;
		}
		else if (!same_change(&dours.v[a], t))
		{
			const ProllyDiff *o = &dours.v[a];
			ProllyConflict *c;

			px_grow((void **) &out->conflicts, &conflict_cap,
					out->nconflicts + 1, sizeof(ProllyConflict));
			c = &out->conflicts[out->nconflicts++];
			/* (copies, the result's: the diffs are freed as the merge ends) */
			c->key = px_memdup(t->key, t->klen);
			c->klen = t->klen;
			c->base = t->from ? px_memdup(t->from, t->flen) : NULL;
			c->blen = t->flen;
			c->ours = o->to ? px_memdup(o->to, o->tlen) : NULL;
			c->olen = o->tlen;
			c->theirs = t->to ? px_memdup(t->to, t->tlen) : NULL;
			c->tlen = t->tlen;
		}
		if (cmp == 0)
			a++;
		b++;
	}
	ta = trace_begin("merge: apply");

	root = *ours;
	for (size_t k = 0; k < grafts.n; k++)
		if (grafts.v[k].level > gtop)
			gtop = grafts.v[k].level;
	for (int level = gtop; level > 0 && rc == PROLLY_OK; level--)
		rc = apply_core(ctx, &root, NULL, 0, &grafts, level, &root);
	if (rc == PROLLY_OK && nedits)
		rc = apply_core(ctx, &root, edits, nedits, NULL, 0, &root);
	if (rc == PROLLY_OK)
		out->root = root;
	trace_end(ta, "%zu edits, %zu grafts on %d levels", nedits, grafts.n, gtop);
	out->ngrafts = grafts.n;
	px_free(edits);
	prolly_diff_list_free(&dours);
	prolly_diff_list_free(&dtheirs);
	graftlist_free(&grafts);
	if (rc != PROLLY_OK)
		prolly_merge_result_free(out);
	return rc;
}

/* The merge, with a memo of the nodes it loads (ctx->memo_bytes of them). */
int
prolly_merge(ProllyCtx *ctx, const PHash *base, const PHash *ours,
			 const PHash *theirs, ProllyMergeResult *out)
{
	NodeMemo	memo = {0};
	volatile int rc;

	if (ctx->memo_bytes == 0 || ctx->memo != NULL)
		return merge_core(ctx, base, ours, theirs, out);
	phmap_init(&memo.map);
	ctx->memo = &memo;
	/* (an error leaves the nodes to its memory's reset, and no memo behind) */
	PG_TRY();
	{
		rc = merge_core(ctx, base, ours, theirs, out);
	}
	PG_FINALLY();
	{
		ctx->memo = NULL;
	}
	PG_END_TRY();
	for (size_t i = 0; i < memo.n; i++)
	{
		memo.nodes[i]->memo = false;
		node_free(memo.nodes[i]);
	}
	px_free(memo.nodes);
	phmap_free(&memo.map);
	return rc;
}

void
prolly_merge_result_free(ProllyMergeResult *r)
{
	for (size_t i = 0; i < r->nconflicts; i++)
	{
		px_free((void *) r->conflicts[i].key);
		px_free((void *) r->conflicts[i].base);
		px_free((void *) r->conflicts[i].ours);
		px_free((void *) r->conflicts[i].theirs);
	}
	px_free(r->conflicts);
	r->conflicts = NULL;
	r->nconflicts = 0;
}

/* --------------------------------------------------------------- deltas */

#define DELTA_PUT 0
#define DELTA_DEL 1

typedef struct DeltaWriter
{
	Buf			b;
	uint64	n;
} DeltaWriter;

static void
delta_entry_fn(void *arg, const ProllyDiff *d)
{
	DeltaWriter *w = arg;

	buf_append_byte(&w->b, d->to ? DELTA_PUT : DELTA_DEL);
	buf_append_uvarint(&w->b, d->klen);
	buf_append(&w->b, d->key, d->klen);
	if (d->to)
	{
		buf_append_uvarint(&w->b, d->tlen);
		buf_append(&w->b, d->to, d->tlen);
	}
	w->n++;
}

int
prolly_delta_write(ProllyCtx *ctx, const PHash *base, const PHash *target,
				   PHash *out)
{
	DeltaWriter w = {BUF_INIT, 0};
	Buf			chunk = BUF_INIT;
	int			rc = prolly_diff(ctx, base, target, delta_entry_fn, &w);

	if (rc == PROLLY_OK)
	{
		buf_append_byte(&chunk, CHUNK_TAG_DELTA);
		buf_append_byte(&chunk, CHUNK_FORMAT_VERSION);
		buf_append(&chunk, base->b, PHASH_LEN);
		buf_append(&chunk, target->b, PHASH_LEN);
		buf_append_uvarint(&chunk, w.n);
		buf_append(&chunk, w.b.data, w.b.len);
		rc = prolly_put_chunk(ctx, chunk.data, chunk.len, out);
	}
	buf_free(&w.b);
	buf_free(&chunk);
	return rc;
}

int
prolly_delta_apply(ProllyCtx *ctx, const PHash *delta, PHash *out)
{
	uint32		len;
	uint8	   *d = cs_get(ctx->cs, delta->b, &len);
	const uint8 *p,
			   *end;
	PHash		base,
				target,
				got;
	uint64		n;
	ProllyEdit *edits = NULL;
	int			rc;

	if (d == NULL)
		return fail(ctx, PROLLY_ERR_MISSING, delta);
	end = d + len;
	if (len < 2 + 2 * PHASH_LEN || d[0] != CHUNK_TAG_DELTA ||
		d[1] != CHUNK_FORMAT_VERSION)
		goto bad;
	p = d + 2;
	memcpy(base.b, p, PHASH_LEN);
	memcpy(target.b, p + PHASH_LEN, PHASH_LEN);
	p += 2 * PHASH_LEN;
	/* (an edit is two bytes at least: its op and its key's length) */
	if (!read_uvarint(&p, end, &n) || n > (uint64) (end - p) / 2)
		goto bad;
	edits = px_malloc(n * sizeof(ProllyEdit));
	for (uint64 i = 0; i < n; i++)
	{
		uint64	kl,
					vl = 0;
		uint8		op;

		if (p >= end || (op = *p++) > DELTA_DEL ||
			!read_uvarint(&p, end, &kl) || kl > (uint64) (end - p))
			goto bad;
		edits[i].key = p;
		edits[i].klen = kl;
		p += kl;
		edits[i].del = op == DELTA_DEL;
		edits[i].val = NULL;
		if (op == DELTA_PUT)
		{
			if (!read_uvarint(&p, end, &vl) || vl > (uint64) (end - p))
				goto bad;
			edits[i].val = p;
			p += vl;
		}
		edits[i].vlen = vl;
	}
	if (p != end)
		goto bad;
	rc = prolly_apply(ctx, &base, edits, n, &got);
	px_free(edits);
	px_free(d);
	if (rc != PROLLY_OK)
		return rc;
	/* the rebuilt tree must be the very tree the delta was taken against */
	if (!phash_eq(&got, &target))
		return fail(ctx, PROLLY_ERR_BAD_DELTA, delta);
	*out = got;
	return PROLLY_OK;
bad:
	px_free(edits);
	px_free(d);
	return fail(ctx, PROLLY_ERR_BAD_DELTA, delta);
}

/* --------------------------------------------------------------- repair */

uint64
prolly_repair(ProllyCtx *ctx, ChunkStore *source, const PHash *root)
{
	return cs_copy_closure(source, ctx->cs, root->b, 1, "repair",
						   "the store repaired from");
}
