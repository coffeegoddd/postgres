/*
 * prolly.c
 *	  Prolly tree nodes, chunking, edits, iteration, diff and merge.
 *
 * Node chunk layout:
 *	byte	NODE_TAG
 *	byte	level (0 = leaf)
 *	uvarint	item count
 *	items:	uvarint klen, key, uvarint vlen, value
 * An internal item's value is the child hash followed by a uvarint count
 * of leaf entries under that child.
 */
#include "prolly.h"

#define NODE_TAG 0x50
#define REF_VAL_MAX (PHASH_LEN + 10)

const ChunkPolicy prolly_default_policy = {1024, 4096, 16384};

/* ---------------------------------------------------------------- nodes */

typedef struct Node
{
	PHash		hash;
	int			level;
	uint32_t	count;
	const uint8_t **k;
	const uint8_t **v;
	uint32_t   *kl;
	uint32_t   *vl;
} Node;

static void
corrupt(const char *what)
{
	fprintf(stderr, "prolly: corrupt chunk: %s\n", what);
	abort();
}

static Node *
node_decode(const PHash *h, const uint8_t *data, size_t len)
{
	const uint8_t *p = data;
	const uint8_t *end = data + len;
	uint64_t	count;
	Node	   *n;
	char	   *mem;

	if (len < 3 || p[0] != NODE_TAG)
		corrupt("bad node header");
	p += 2;
	if (!read_uvarint(&p, end, &count) || count > len)
		corrupt("bad node count");
	mem = px_malloc(sizeof(Node) +
					count * (2 * sizeof(const uint8_t *) + 2 * sizeof(uint32_t)));
	n = (Node *) mem;
	n->hash = *h;
	n->level = data[1];
	n->count = (uint32_t) count;
	n->k = (const uint8_t **) (mem + sizeof(Node));
	n->v = n->k + count;
	n->kl = (uint32_t *) (n->v + count);
	n->vl = n->kl + count;
	for (uint32_t i = 0; i < n->count; i++)
	{
		uint64_t	kl,
					vl;

		if (!read_uvarint(&p, end, &kl) || kl > (uint64_t) (end - p))
			corrupt("bad key");
		n->k[i] = p;
		n->kl[i] = (uint32_t) kl;
		p += kl;
		if (!read_uvarint(&p, end, &vl) || vl > (uint64_t) (end - p))
			corrupt("bad value");
		n->v[i] = p;
		n->vl[i] = (uint32_t) vl;
		p += vl;
	}
	if (p != end)
		corrupt("trailing bytes");
	return n;
}

static Node *
node_load(ProllyCtx *ctx, const PHash *h)
{
	size_t		len;
	const uint8_t *data = cs_get(ctx->cs, h, &len);

	if (data == NULL)
	{
		char		hex[PHASH_HEX_LEN + 1];

		phash_hex(h, hex);
		fprintf(stderr, "prolly: missing chunk %s\n", hex);
		abort();
	}
	return node_decode(h, data, len);
}

static void
node_free(Node *n)
{
	free(n);
}

static size_t
ref_encode(uint8_t *out, const PHash *h, uint64_t total)
{
	size_t		n = PHASH_LEN;

	memcpy(out, h->b, PHASH_LEN);
	while (total >= 0x80)
	{
		out[n++] = (uint8_t) (total | 0x80);
		total >>= 7;
	}
	out[n++] = (uint8_t) total;
	return n;
}

static void
ref_decode(const uint8_t *v, size_t vl, PHash *h, uint64_t *total)
{
	const uint8_t *p = v + PHASH_LEN;

	if (vl <= PHASH_LEN)
		corrupt("bad child ref");
	memcpy(h->b, v, PHASH_LEN);
	if (!read_uvarint(&p, v + vl, total) || p != v + vl)
		corrupt("bad child count");
}

static void
node_child(const Node *n, uint32_t i, PHash *h, uint64_t *total)
{
	ref_decode(n->v[i], n->vl[i], h, total);
}

static uint64_t
node_total(const Node *n)
{
	uint64_t	sum = 0;

	if (n->level == 0)
		return n->count;
	for (uint32_t i = 0; i < n->count; i++)
	{
		PHash		h;
		uint64_t	t;

		node_child(n, i, &h, &t);
		sum += t;
	}
	return sum;
}

/* First index whose key is >= key; count if none. */
static uint32_t
node_search(const Node *n, const uint8_t *key, size_t klen)
{
	uint32_t	lo = 0,
				hi = n->count;

	while (lo < hi)
	{
		uint32_t	mid = lo + (hi - lo) / 2;

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
	uint8_t    *key;			/* last key in the node (owned) */
	uint32_t	klen;
	PHash		hash;
	uint64_t	total;
} NodeRef;

typedef struct RefList
{
	NodeRef    *v;
	size_t		n;
	size_t		cap;
} RefList;

static void
reflist_push(RefList *l, const uint8_t *key, uint32_t klen, const PHash *h,
			 uint64_t total)
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
		free(l->v[i].key);
	free(l->v);
	memset(l, 0, sizeof(*l));
}

static uint64_t
key_hash64(int level, const uint8_t *k, size_t kl)
{
	uint64_t	h = 0xcbf29ce484222325ULL ^ ((uint64_t) (level + 1) * 0x9e3779b97f4a7c15ULL);

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
policy_boundary(const ChunkPolicy *p, int level, const uint8_t *k, size_t kl,
				size_t item_bytes, size_t chunk_bytes)
{
	uint64_t	span,
				thr;

	if (chunk_bytes < p->min_bytes)
		return false;
	if (chunk_bytes >= p->max_bytes)
		return true;
	span = p->target_bytes - p->min_bytes;
	thr = item_bytes >= span ? UINT64_MAX : (UINT64_MAX / span) * item_bytes;
	return key_hash64(level, k, kl) < thr;
}

/* Packs a stream of items at one level into nodes. */
typedef struct LevelChunker
{
	ProllyCtx  *ctx;
	int			level;
	Buf			items;
	uint32_t	nitems;
	uint64_t	total;
	size_t		lastkoff;
	uint32_t	lastklen;
	Buf			scratch;
	RefList    *out;
} LevelChunker;

static void
lc_init(LevelChunker *lc, ProllyCtx *ctx, int level, RefList *out)
{
	memset(lc, 0, sizeof(*lc));
	lc->ctx = ctx;
	lc->level = level;
	lc->out = out;
}

static PHash
write_node(ProllyCtx *ctx, Buf *scratch, int level, uint32_t nitems,
		   const uint8_t *items, size_t ilen)
{
	scratch->len = 0;
	buf_append_byte(scratch, NODE_TAG);
	buf_append_byte(scratch, (uint8_t) level);
	buf_append_uvarint(scratch, nitems);
	buf_append(scratch, items, ilen);
	return cs_put_chunk(ctx->cs, scratch->data, scratch->len);
}

static void
lc_flush(LevelChunker *lc)
{
	PHash		h;

	if (lc->nitems == 0)
		return;
	h = write_node(lc->ctx, &lc->scratch, lc->level, lc->nitems,
				   lc->items.data, lc->items.len);
	reflist_push(lc->out, lc->items.data + lc->lastkoff, lc->lastklen, &h,
				 lc->total);
	lc->items.len = 0;
	lc->nitems = 0;
	lc->total = 0;
}

static void
lc_add(LevelChunker *lc, const uint8_t *k, size_t kl, const uint8_t *v, size_t vl)
{
	size_t		before = lc->items.len;

	buf_append_uvarint(&lc->items, kl);
	lc->lastkoff = lc->items.len;
	lc->lastklen = (uint32_t) kl;
	buf_append(&lc->items, k, kl);
	buf_append_uvarint(&lc->items, vl);
	buf_append(&lc->items, v, vl);
	lc->nitems++;
	if (lc->level == 0)
		lc->total++;
	else
	{
		PHash		h;
		uint64_t	t;

		ref_decode(v, vl, &h, &t);
		lc->total += t;
	}
	if (policy_boundary(&lc->ctx->policy, lc->level, k, kl,
						lc->items.len - before, lc->items.len))
		lc_flush(lc);
}

static void
lc_free(LevelChunker *lc)
{
	buf_free(&lc->items);
	buf_free(&lc->scratch);
}

PHash
prolly_empty(ProllyCtx *ctx)
{
	Buf			scratch = BUF_INIT;
	PHash		h = write_node(ctx, &scratch, 0, 0, NULL, 0);

	buf_free(&scratch);
	return h;
}

/* Builds levels above refs (nodes at level-1) until one node remains. */
static PHash
build_up(ProllyCtx *ctx, RefList *refs, int level)
{
	PHash		root;

	while (refs->n > 1)
	{
		RefList		next = {0};
		LevelChunker lc;
		uint8_t		val[REF_VAL_MAX];

		lc_init(&lc, ctx, level, &next);
		for (size_t i = 0; i < refs->n; i++)
		{
			size_t		vl = ref_encode(val, &refs->v[i].hash, refs->v[i].total);

			lc_add(&lc, refs->v[i].key, refs->v[i].klen, val, vl);
		}
		lc_flush(&lc);
		lc_free(&lc);
		reflist_free(refs);
		*refs = next;
		level++;
	}
	root = refs->n == 0 ? prolly_empty(ctx) : refs->v[0].hash;
	reflist_free(refs);
	return root;
}

PHash
prolly_build(ProllyCtx *ctx, const ProllyKV *kvs, size_t n)
{
	RefList		refs = {0};
	LevelChunker lc;

	lc_init(&lc, ctx, 0, &refs);
	for (size_t i = 0; i < n; i++)
		lc_add(&lc, kvs[i].key, kvs[i].klen, kvs[i].val, kvs[i].vlen);
	lc_flush(&lc);
	lc_free(&lc);
	return build_up(ctx, &refs, 1);
}

/* --------------------------------------------------------------- cursor */

/*
 * A cursor holds the path of loaded nodes from the root down to level d and
 * points at entry i[d] of n[d].  Above the leaves that entry is a child that
 * has not been loaded yet, which lets diff skip equal subtrees unread.
 */
typedef struct Cursor
{
	ProllyCtx  *ctx;
	int			top;
	int			d;
	bool		end;
	Node	   *n[PROLLY_MAX_HEIGHT];
	uint32_t	i[PROLLY_MAX_HEIGHT];
} Cursor;

static void
cursor_open(Cursor *c, ProllyCtx *ctx, const PHash *root)
{
	Node	   *r = node_load(ctx, root);

	memset(c, 0, sizeof(*c));
	c->ctx = ctx;
	if (r->level >= PROLLY_MAX_HEIGHT)
		corrupt("tree too tall");
	c->top = c->d = r->level;
	c->n[c->top] = r;
	c->end = r->count == 0;
}

static void
cursor_close(Cursor *c)
{
	for (int l = 0; l <= c->top; l++)
		if (c->n[l])
			node_free(c->n[l]);
	memset(c->n, 0, sizeof(c->n));
}

static void
cursor_descend(Cursor *c)
{
	PHash		h;
	uint64_t	t;

	node_child(c->n[c->d], c->i[c->d], &h, &t);
	c->d--;
	if (c->n[c->d])
		node_free(c->n[c->d]);
	c->n[c->d] = node_load(c->ctx, &h);
	c->i[c->d] = 0;
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

static void
cursor_next_item(Cursor *c)
{
	cursor_advance(c);
	while (!c->end && c->d > 0)
		cursor_descend(c);
}

/* Positions the cursor in the node at `level` that would hold key. */
static void
cursor_seek(Cursor *c, ProllyCtx *ctx, const PHash *root, int level,
			const uint8_t *key, size_t klen)
{
	cursor_open(c, ctx, root);
	if (c->end)
		return;
	while (c->d > level)
	{
		Node	   *n = c->n[c->d];
		uint32_t	i = key ? node_search(n, key, klen) : 0;

		c->i[c->d] = i < n->count ? i : n->count - 1;
		cursor_descend(c);
	}
	c->i[level] = key ? node_search(c->n[level], key, klen) : 0;
}

static bool
cursor_is_last_node(const Cursor *c, int level)
{
	for (int l = level + 1; l <= c->top; l++)
		if (c->i[l] + 1 < c->n[l]->count)
			return false;
	return true;
}

static bool
cursor_next_node(Cursor *c, int level)
{
	c->i[level] = c->n[level]->count - 1;
	cursor_advance(c);
	if (c->end)
		return false;
	while (c->d > level)
		cursor_descend(c);
	return true;
}

/* ---------------------------------------------------------------- reads */

bool
prolly_get(ProllyCtx *ctx, const PHash *root, const uint8_t *key, size_t klen,
		   const uint8_t **val, size_t *vlen)
{
	Node	   *n = node_load(ctx, root);
	uint32_t	i;
	bool		found;

	while (n->level > 0)
	{
		PHash		h;
		uint64_t	t;

		i = node_search(n, key, klen);
		if (i >= n->count)
		{
			node_free(n);
			return false;
		}
		node_child(n, i, &h, &t);
		node_free(n);
		n = node_load(ctx, &h);
	}
	i = node_search(n, key, klen);
	found = i < n->count && bytes_eq(n->k[i], n->kl[i], key, klen);
	if (found)
	{
		*val = n->v[i];
		*vlen = n->vl[i];
	}
	node_free(n);
	return found;
}

uint64_t
prolly_count(ProllyCtx *ctx, const PHash *root)
{
	Node	   *n = node_load(ctx, root);
	uint64_t	t = node_total(n);

	node_free(n);
	return t;
}

int
prolly_height(ProllyCtx *ctx, const PHash *root)
{
	Node	   *n = node_load(ctx, root);
	int			h = n->level + 1;

	node_free(n);
	return h;
}

struct ProllyIter
{
	Cursor		c;
};

ProllyIter *
prolly_iter_open(ProllyCtx *ctx, const PHash *root, const uint8_t *start,
				 size_t slen)
{
	ProllyIter *it = px_malloc(sizeof(ProllyIter));

	cursor_seek(&it->c, ctx, root, 0, start, slen);
	if (!it->c.end && it->c.i[0] >= it->c.n[0]->count)
	{
		it->c.i[0] = it->c.n[0]->count - 1;
		cursor_next_item(&it->c);
	}
	return it;
}

bool
prolly_iter_next(ProllyIter *it, const uint8_t **key, size_t *klen,
				 const uint8_t **val, size_t *vlen)
{
	Cursor	   *c = &it->c;
	Node	   *n;

	if (c->end)
		return false;
	n = c->n[0];
	*key = n->k[c->i[0]];
	*klen = n->kl[c->i[0]];
	*val = n->v[c->i[0]];
	*vlen = n->vl[c->i[0]];
	cursor_next_item(c);
	return true;
}

void
prolly_iter_close(ProllyIter *it)
{
	cursor_close(&it->c);
	free(it);
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
editbuf_add(EditBuf *eb, const uint8_t *k, size_t kl, const uint8_t *v,
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
	free(eb->v);
	memset(eb, 0, sizeof(*eb));
}

typedef struct OldRef
{
	const uint8_t *key;
	uint32_t	klen;
	PHash		hash;
} OldRef;

/*
 * Applies sorted, unique edits to the items at one level of the tree at
 * root.  For each run of edits, rechunks from the start of the first node
 * the run touches and keeps going until a new boundary lands on an old one;
 * from there on the old nodes are reused as they are.  The replaced nodes
 * become edits to the level above (written to parent), or, at the root
 * level, the new nodes are returned in top_refs.
 */
static void
level_apply(ProllyCtx *ctx, const PHash *root, int level,
			const ProllyEdit *ed, size_t ne, EditBuf *parent, RefList *top_refs)
{
	size_t		e = 0;
	OldRef	   *old = NULL;
	size_t		oldcap = 0;

	while (e < ne)
	{
		Cursor		c;
		RefList		fresh = {0};
		LevelChunker lc;
		size_t		nold = 0;

		cursor_seek(&c, ctx, root, level, ed[e].key, ed[e].klen);
		lc_init(&lc, ctx, level, &fresh);
		for (;;)
		{
			Node	   *nd = c.n[level];
			bool		last = cursor_is_last_node(&c, level);
			const uint8_t *lk = nd->count ? nd->k[nd->count - 1] : NULL;
			uint32_t	lkl = nd->count ? nd->kl[nd->count - 1] : 0;
			uint32_t	j = 0;

			for (;;)
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
					lc_add(&lc, nd->k[j], nd->kl[j], nd->v[j], nd->vl[j]);
					j++;
				}
				else
				{
					if (!ed[e].del)
						lc_add(&lc, ed[e].key, ed[e].klen, ed[e].val, ed[e].vlen);
					if (cmp == 0)
						j++;
					e++;
				}
			}
			if (nd->count)
			{
				px_grow((void **) &old, &oldcap, nold + 1, sizeof(OldRef));
				old[nold].key = lk;
				old[nold].klen = lkl;
				old[nold].hash = nd->hash;
				nold++;
			}
			if (last)
			{
				lc_flush(&lc);
				break;
			}
			if (lc.nitems == 0)
				break;			/* resynchronized with the old boundaries */
			cursor_next_node(&c, level);
		}
		lc_free(&lc);

		if (top_refs)
		{
			for (size_t i = 0; i < fresh.n; i++)
				reflist_push(top_refs, fresh.v[i].key, fresh.v[i].klen,
							 &fresh.v[i].hash, fresh.v[i].total);
		}
		else
		{
			size_t		a = 0,
						b = 0;
			uint8_t		val[REF_VAL_MAX];

			while (a < nold || b < fresh.n)
			{
				int			cmp = a >= nold ? 1 : b >= fresh.n ? -1 :
					key_cmp(old[a].key, old[a].klen, fresh.v[b].key, fresh.v[b].klen);

				if (cmp < 0)
				{
					editbuf_add(parent, old[a].key, old[a].klen, NULL, 0, true);
					a++;
				}
				else
				{
					NodeRef    *r = &fresh.v[b];

					if (cmp > 0 || !phash_eq(&old[a].hash, &r->hash))
						editbuf_add(parent, r->key, r->klen, val,
									ref_encode(val, &r->hash, r->total), false);
					if (cmp == 0)
						a++;
					b++;
				}
			}
		}
		/* old[] keys point at chunk bytes, which outlive the nodes */
		cursor_close(&c);
		reflist_free(&fresh);
	}
	free(old);
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
static PHash
collapse(ProllyCtx *ctx, PHash root)
{
	for (;;)
	{
		Node	   *n = node_load(ctx, &root);
		uint64_t	t;

		if (n->level == 0 || n->count != 1)
		{
			node_free(n);
			return root;
		}
		node_child(n, 0, &root, &t);
		node_free(n);
	}
}

PHash
prolly_apply(ProllyCtx *ctx, const PHash *root, const ProllyEdit *edits, size_t n)
{
	SortEdit   *s = px_malloc(n * sizeof(SortEdit));
	ProllyEdit *cur = px_malloc(n * sizeof(ProllyEdit));
	size_t		ncur = 0;
	EditBuf		eb = {0};
	PHash		result = *root;
	int			top;

	for (size_t i = 0; i < n; i++)
	{
		s[i].e = edits[i];
		s[i].seq = i;
	}
	qsort(s, n, sizeof(SortEdit), sortedit_cmp);
	for (size_t i = 0; i < n; i++)
	{
		if (i + 1 < n && key_cmp(s[i].e.key, s[i].e.klen,
								 s[i + 1].e.key, s[i + 1].e.klen) == 0)
			continue;			/* a later edit of the same key wins */
		cur[ncur++] = s[i].e;
	}
	free(s);

	top = prolly_height(ctx, root) - 1;
	for (int level = 0; ncur > 0; level++)
	{
		if (level < top)
		{
			EditBuf		next = {0};

			level_apply(ctx, root, level, cur, ncur, &next, NULL);
			free(cur);
			editbuf_free(&eb);
			eb = next;
			cur = editbuf_edits(&eb);
			ncur = eb.n;
		}
		else
		{
			RefList		refs = {0};

			level_apply(ctx, root, level, cur, ncur, NULL, &refs);
			if (refs.n == 0)
			{
				result = prolly_empty(ctx);
				reflist_free(&refs);
			}
			else if (refs.n == 1)
			{
				result = collapse(ctx, refs.v[0].hash);
				reflist_free(&refs);
			}
			else
				result = build_up(ctx, &refs, level + 1);
			break;
		}
	}
	free(cur);
	editbuf_free(&eb);
	return result;
}

/* ----------------------------------------------------------------- diff */

static void
emit_item(Cursor *c, bool removed, ProllyDiffFn fn, void *arg)
{
	Node	   *n = c->n[0];
	uint32_t	i = c->i[0];
	ProllyDiff	d = {n->k[i], n->kl[i], NULL, 0, NULL, 0};

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

static void
drain(Cursor *c, bool removed, ProllyDiffFn fn, void *arg)
{
	while (!c->end)
	{
		if (c->d > 0)
		{
			cursor_descend(c);
			continue;
		}
		emit_item(c, removed, fn, arg);
		cursor_advance(c);
	}
}

void
prolly_diff(ProllyCtx *ctx, const PHash *from, const PHash *to,
			ProllyDiffFn fn, void *arg)
{
	Cursor		a,
				b;

	if (phash_eq(from, to))
		return;
	cursor_open(&a, ctx, from);
	cursor_open(&b, ctx, to);
	while (!a.end && !b.end)
	{
		int			da = a.d,
					db = b.d;

		if (da > 0 || db > 0)
		{
			if (da == db)
			{
				PHash		ha,
							hb;
				uint64_t	t;

				node_child(a.n[da], a.i[da], &ha, &t);
				node_child(b.n[db], b.i[db], &hb, &t);
				if (phash_eq(&ha, &hb))
				{
					/* identical subtrees: skip both without reading them */
					cursor_advance(&a);
					cursor_advance(&b);
					continue;
				}
			}
			if (da >= db && da > 0)
				cursor_descend(&a);
			if (db >= da && db > 0)
				cursor_descend(&b);
			continue;
		}
		else
		{
			Node	   *na = a.n[0],
					   *nb = b.n[0];
			uint32_t	ia = a.i[0],
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
				if (!bytes_eq(na->v[ia], na->vl[ia], nb->v[ib], nb->vl[ib]))
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
	drain(&a, true, fn, arg);
	drain(&b, false, fn, arg);
	cursor_close(&a);
	cursor_close(&b);
}

static void
collect_fn(void *arg, const ProllyDiff *d)
{
	ProllyDiffList *l = arg;

	px_grow((void **) &l->v, &l->cap, l->n + 1, sizeof(ProllyDiff));
	l->v[l->n++] = *d;
}

void
prolly_diff_collect(ProllyCtx *ctx, const PHash *from, const PHash *to,
					ProllyDiffList *out)
{
	memset(out, 0, sizeof(*out));
	prolly_diff(ctx, from, to, collect_fn, out);
}

void
prolly_diff_list_free(ProllyDiffList *l)
{
	free(l->v);
	memset(l, 0, sizeof(*l));
}

/* ---------------------------------------------------------------- merge */

static bool
same_change(const ProllyDiff *a, const ProllyDiff *b)
{
	if (a->to == NULL || b->to == NULL)
		return a->to == NULL && b->to == NULL;
	return bytes_eq(a->to, a->tlen, b->to, b->tlen);
}

void
prolly_merge(ProllyCtx *ctx, const PHash *base, const PHash *ours,
			 const PHash *theirs, ProllyMergeResult *out)
{
	ProllyDiffList dours,
				dtheirs;
	ProllyEdit *edits;
	size_t		nedits = 0,
				conflict_cap = 0,
				a = 0,
				b = 0;

	memset(out, 0, sizeof(*out));
	if (phash_eq(ours, theirs) || phash_eq(base, theirs))
	{
		out->root = *ours;
		return;
	}
	if (phash_eq(base, ours))
	{
		out->root = *theirs;
		return;
	}
	prolly_diff_collect(ctx, base, ours, &dours);
	prolly_diff_collect(ctx, base, theirs, &dtheirs);
	edits = px_malloc(dtheirs.n * sizeof(ProllyEdit));
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
			c->key = t->key;
			c->klen = t->klen;
			c->base = t->from;
			c->blen = t->flen;
			c->ours = o->to;
			c->olen = o->tlen;
			c->theirs = t->to;
			c->tlen = t->tlen;
		}
		if (cmp == 0)
			a++;
		b++;
	}
	out->root = nedits ? prolly_apply(ctx, ours, edits, nedits) : *ours;
	free(edits);
	prolly_diff_list_free(&dours);
	prolly_diff_list_free(&dtheirs);
}

void
prolly_merge_result_free(ProllyMergeResult *r)
{
	free(r->conflicts);
	memset(r, 0, sizeof(*r));
}

/* ----------------------------------------------------------------- sync */

void
prolly_sync(ProllyCtx *src, ChunkStore *dst, const PHash *root,
			ProllyLeafFn leaf_fn, void *arg)
{
	Node	   *n;
	const uint8_t *data;
	size_t		len;

	if (cs_has(dst, root))
		return;
	n = node_load(src, root);
	for (uint32_t i = 0; i < n->count; i++)
	{
		if (n->level > 0)
		{
			PHash		h;
			uint64_t	t;

			node_child(n, i, &h, &t);
			prolly_sync(src, dst, &h, leaf_fn, arg);
		}
		else if (leaf_fn)
			leaf_fn(arg, n->k[i], n->kl[i], n->v[i], n->vl[i]);
	}
	node_free(n);
	data = cs_get(src->cs, root, &len);
	cs_put(dst, root, data, len);
}
