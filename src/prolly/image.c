/*
 * image.c
 *	  Commit images: page chunks, the page and relation maps, building an
 *	  image on top of a base image, reading and diffing images.
 *
 * Page map key: relation id (4 bytes, big-endian), fork (1 byte), block
 * (4 bytes, big-endian), so a relation's pages are contiguous and in
 * block order.  Value: the page chunk's hash, held strongly (vrefs 1).
 *
 * Relation map key: relation id (4 bytes, big-endian).  Value: kind byte,
 * uvarint parent, uvarint name length, name, uvarint block count per
 * fork, uvarint catalog length, catalog bytes.
 */
#include "image.h"

#define PAGE_KEY_LEN 9
#define REL_KEY_LEN 4

static void
put_be32(uint8_t *o, uint32_t v)
{
	o[0] = (uint8_t) (v >> 24);
	o[1] = (uint8_t) (v >> 16);
	o[2] = (uint8_t) (v >> 8);
	o[3] = (uint8_t) v;
}

static uint32_t
get_be32(const uint8_t *p)
{
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 |
		(uint32_t) p[2] << 8 | p[3];
}

static void
page_key(uint8_t *k, uint32_t relid, int fork, uint32_t block)
{
	put_be32(k, relid);
	k[4] = (uint8_t) fork;
	put_be32(k + 5, block);
}

static int
fail(ProllyCtx *ctx, int err, const PHash *h)
{
	ctx->err = err;
	if (h)
		ctx->err_hash = *h;
	return err;
}

/* ---------------------------------------------------------- descriptors */

static void
rel_encode(Buf *b, const ImgRelation *r)
{
	buf_append_byte(b, (uint8_t) r->kind);
	buf_append_uvarint(b, r->parent);
	buf_append_uvarint(b, r->namelen);
	buf_append(b, r->name, r->namelen);
	for (int f = 0; f < IMG_NFORKS; f++)
		buf_append_uvarint(b, r->nblocks[f]);
	buf_append_uvarint(b, r->catlen);
	buf_append(b, r->catalog, r->catlen);
}

static bool
rel_decode(uint32_t relid, const uint8_t *v, size_t vl, ImgRelation *r)
{
	const uint8_t *p = v;
	const uint8_t *end = v + vl;
	uint64_t	x;

	memset(r, 0, sizeof(*r));
	r->relid = relid;
	if (p >= end)
		return false;
	r->kind = (ImgRelKind) * p++;
	if (!read_uvarint(&p, end, &x) || x > UINT32_MAX)
		return false;
	r->parent = (uint32_t) x;
	if (!read_uvarint(&p, end, &x) || x > (uint64_t) (end - p))
		return false;
	r->name = (const char *) p;
	r->namelen = x;
	p += x;
	for (int f = 0; f < IMG_NFORKS; f++)
	{
		if (!read_uvarint(&p, end, &x) || x > UINT32_MAX)
			return false;
		r->nblocks[f] = (uint32_t) x;
	}
	if (!read_uvarint(&p, end, &x) || x != (uint64_t) (end - p))
		return false;
	r->catalog = p;
	r->catlen = x;
	return true;
}

static bool
kind_valid(ImgRelKind k)
{
	return k == IMG_REL_TABLE || k == IMG_REL_INDEX || k == IMG_REL_TOAST ||
		k == IMG_REL_SEQUENCE;
}

/* ---------------------------------------------------------------- header */

int
img_load(ProllyCtx *ctx, const PHash *image, ImgHeader *h)
{
	size_t		len;
	const uint8_t *d = cs_get(ctx->cs, image, &len);
	const uint8_t *p,
			   *end;
	uint64_t	v[3],
				nxip;

	memset(h, 0, sizeof(*h));
	if (d == NULL)
		return fail(ctx, PROLLY_ERR_MISSING, image);
	if (len < 2 || d[0] != CHUNK_TAG_IMAGE || d[1] != CHUNK_FORMAT_VERSION)
		return fail(ctx, PROLLY_ERR_CORRUPT, image);
	p = d + 2;
	end = d + len;
	h->hash = *image;
	for (int i = 0; i < 3; i++)
		if (!read_uvarint(&p, end, &v[i]) || v[i] > UINT32_MAX)
			return fail(ctx, PROLLY_ERR_CORRUPT, image);
	h->format.pg_major = (uint32_t) v[0];
	h->format.page_size = (uint32_t) v[1];
	h->format.catversion = (uint32_t) v[2];
	if (end - p < 8 + PHASH_LEN + 1)
		return fail(ctx, PROLLY_ERR_CORRUPT, image);
	for (int i = 0; i < 8; i++)
		h->format.system_id = h->format.system_id << 8 | p[i];
	p += 8;
	memcpy(h->commit.b, p, PHASH_LEN);
	p += PHASH_LEN;
	h->has_base = *p++ != 0;
	if (h->has_base)
	{
		if (end - p < PHASH_LEN)
			return fail(ctx, PROLLY_ERR_CORRUPT, image);
		memcpy(h->base.b, p, PHASH_LEN);
		p += PHASH_LEN;
	}
	if (!read_uvarint(&p, end, &h->lsn) ||
		!read_uvarint(&p, end, &h->snapshot.xmin) ||
		!read_uvarint(&p, end, &h->snapshot.xmax) ||
		!read_uvarint(&p, end, &nxip) || nxip > len)
		return fail(ctx, PROLLY_ERR_CORRUPT, image);
	h->snapshot.xip = px_malloc(nxip * sizeof(uint64_t));
	h->snapshot.nxip = nxip;
	for (uint64_t i = 0; i < nxip; i++)
		if (!read_uvarint(&p, end, &h->snapshot.xip[i]))
			goto bad;
	if (end - p != 2 * PHASH_LEN)
		goto bad;
	memcpy(h->pages.b, p, PHASH_LEN);
	memcpy(h->rels.b, p + PHASH_LEN, PHASH_LEN);
	return PROLLY_OK;
bad:
	img_header_free(h);
	return fail(ctx, PROLLY_ERR_CORRUPT, image);
}

void
img_header_free(ImgHeader *h)
{
	free(h->snapshot.xip);
	h->snapshot.xip = NULL;
	h->snapshot.nxip = 0;
}

bool
img_compatible(const ImgHeader *h, const ImgFormat *f)
{
	return h->format.pg_major == f->pg_major &&
		h->format.page_size == f->page_size &&
		h->format.catversion == f->catversion &&
		h->format.system_id == f->system_id;
}

/* --------------------------------------------------------------- builder */

typedef struct PageEdit
{
	uint8_t		key[PAGE_KEY_LEN];
	PHash		page;
	bool		del;
	size_t		seq;
} PageEdit;

typedef struct RelEdit
{
	uint32_t	relid;
	bool		del;
	size_t		off;
	size_t		len;
	size_t		seq;
} RelEdit;

struct ImgBuilder
{
	ProllyCtx  *ctx;
	ImgFormat	format;
	bool		has_base;
	ImgHeader	base;
	PageEdit   *pe;
	size_t		npe;
	size_t		pecap;
	RelEdit    *re;
	size_t		nre;
	size_t		recap;
	Buf			arena;
	size_t		seq;
};

int
img_begin(ProllyCtx *ctx, const ImgFormat *format, const PHash *base,
		  ImgBuilder **out)
{
	ImgBuilder *b = px_malloc(sizeof(ImgBuilder));

	memset(b, 0, sizeof(*b));
	b->ctx = ctx;
	b->format = *format;
	if (format->page_size == 0)
	{
		free(b);
		return fail(ctx, IMG_ERR_INVALID, NULL);
	}
	if (base)
	{
		int			rc = img_load(ctx, base, &b->base);

		if (rc != PROLLY_OK)
		{
			free(b);
			return rc;
		}
		if (!img_compatible(&b->base, format))
		{
			img_header_free(&b->base);
			free(b);
			return fail(ctx, IMG_ERR_FORMAT, base);
		}
		b->has_base = true;
	}
	*out = b;
	return PROLLY_OK;
}

void
img_abort(ImgBuilder *b)
{
	if (b->has_base)
		img_header_free(&b->base);
	free(b->pe);
	free(b->re);
	buf_free(&b->arena);
	free(b);
}

int
img_put_relation(ImgBuilder *b, const ImgRelation *rel)
{
	RelEdit    *e;

	if (!kind_valid(rel->kind))
		return fail(b->ctx, IMG_ERR_INVALID, NULL);
	px_grow((void **) &b->re, &b->recap, b->nre + 1, sizeof(RelEdit));
	e = &b->re[b->nre++];
	e->relid = rel->relid;
	e->del = false;
	e->off = b->arena.len;
	rel_encode(&b->arena, rel);
	e->len = b->arena.len - e->off;
	e->seq = b->seq++;
	return PROLLY_OK;
}

int
img_drop_relation(ImgBuilder *b, uint32_t relid)
{
	RelEdit    *e;

	px_grow((void **) &b->re, &b->recap, b->nre + 1, sizeof(RelEdit));
	e = &b->re[b->nre++];
	memset(e, 0, sizeof(*e));
	e->relid = relid;
	e->del = true;
	e->seq = b->seq++;
	return PROLLY_OK;
}

static void
add_page_edit(ImgBuilder *b, uint32_t relid, int fork, uint32_t block,
			  const PHash *page, bool del)
{
	PageEdit   *e;

	px_grow((void **) &b->pe, &b->pecap, b->npe + 1, sizeof(PageEdit));
	e = &b->pe[b->npe++];
	page_key(e->key, relid, fork, block);
	if (page)
		e->page = *page;
	e->del = del;
	e->seq = b->seq++;
}

int
img_put_page(ImgBuilder *b, uint32_t relid, int fork, uint32_t block,
			 const uint8_t *page)
{
	Buf			chunk = BUF_INIT;
	PHash		h;
	int			rc;

	if (fork < 0 || fork >= IMG_NFORKS)
		return fail(b->ctx, IMG_ERR_INVALID, NULL);
	buf_append_byte(&chunk, CHUNK_TAG_PAGE);
	buf_append_byte(&chunk, CHUNK_FORMAT_VERSION);
	buf_append(&chunk, page, b->format.page_size);
	rc = prolly_put_chunk(b->ctx, chunk.data, chunk.len, &h);
	buf_free(&chunk);
	if (rc != PROLLY_OK)
		return rc;
	add_page_edit(b, relid, fork, block, &h, false);
	return PROLLY_OK;
}

static int
seq_cmp(const void *pa, const void *pb, size_t keylen, size_t seqa, size_t seqb)
{
	int			c = memcmp(pa, pb, keylen);

	if (c)
		return c;
	return (seqa > seqb) - (seqa < seqb);
}

static int
page_edit_cmp(const void *a, const void *b)
{
	const PageEdit *x = a,
			   *y = b;

	return seq_cmp(x->key, y->key, PAGE_KEY_LEN, x->seq, y->seq);
}

static int
rel_edit_cmp(const void *a, const void *b)
{
	const RelEdit *x = a,
			   *y = b;

	if (x->relid != y->relid)
		return x->relid < y->relid ? -1 : 1;
	return (x->seq > y->seq) - (x->seq < y->seq);
}

/* A relation's state before and after this builder. */
typedef struct RelState
{
	uint32_t	relid;
	bool		before;
	uint32_t	old_n[IMG_NFORKS];
	bool		after;
	uint32_t	new_n[IMG_NFORKS];
	const RelEdit *edit;		/* its last edit here, if any */
} RelState;

static int
base_relation(ImgBuilder *b, uint32_t relid, ImgRelation *out, bool *found)
{
	uint8_t		k[REL_KEY_LEN];
	const uint8_t *v;
	size_t		vl;
	int			rc;

	*found = false;
	if (!b->has_base)
		return PROLLY_OK;
	put_be32(k, relid);
	rc = prolly_get(b->ctx, &b->base.rels, k, REL_KEY_LEN, &v, &vl);
	if (rc == PROLLY_NOT_FOUND)
		return PROLLY_OK;
	if (rc != PROLLY_OK)
		return rc;
	if (!rel_decode(relid, v, vl, out))
		return fail(b->ctx, PROLLY_ERR_CORRUPT, &b->base.rels);
	*found = true;
	return PROLLY_OK;
}

static RelState *
find_state(RelState *st, size_t n, uint32_t relid)
{
	size_t		lo = 0,
				hi = n;

	while (lo < hi)
	{
		size_t		mid = lo + (hi - lo) / 2;

		if (st[mid].relid == relid)
			return &st[mid];
		if (st[mid].relid < relid)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

static int
state_cmp(const void *a, const void *b)
{
	const RelState *x = a,
			   *y = b;

	return (x->relid > y->relid) - (x->relid < y->relid);
}

/*
 * Works out each touched relation's block counts before and after, drops
 * pages a relation no longer has, and checks every page edit against the
 * relation it belongs to.  O(changes): untouched relations and pages are
 * never visited.
 */
static int
resolve(ImgBuilder *b, RelState **out, size_t *nout)
{
	RelState   *st = NULL;
	size_t		n = 0,
				cap = 0;
	int			rc = PROLLY_OK;
	size_t		npe_given;

	if (b->nre > 1)
		qsort(b->re, b->nre, sizeof(RelEdit), rel_edit_cmp);
	for (size_t i = 0; i < b->nre; i++)
		if (i + 1 == b->nre || b->re[i + 1].relid != b->re[i].relid)
		{
			px_grow((void **) &st, &cap, n + 1, sizeof(RelState));
			memset(&st[n], 0, sizeof(RelState));
			st[n].relid = b->re[i].relid;
			st[n].edit = &b->re[i];
			n++;
		}
	/* relations whose pages change without a new description */
	if (b->npe > 1)
		qsort(b->pe, b->npe, sizeof(PageEdit), page_edit_cmp);
	for (size_t i = 0; i < b->npe; i++)
	{
		uint32_t	relid = get_be32(b->pe[i].key);

		if (find_state(st, n, relid) == NULL &&
			(n == 0 || st[n - 1].relid != relid))
		{
			px_grow((void **) &st, &cap, n + 1, sizeof(RelState));
			memset(&st[n], 0, sizeof(RelState));
			st[n].relid = relid;
			n++;
			if (n > 1)
				qsort(st, n, sizeof(RelState), state_cmp);
		}
	}

	for (size_t i = 0; i < n && rc == PROLLY_OK; i++)
	{
		RelState   *s = &st[i];
		ImgRelation r;
		bool		found;

		rc = base_relation(b, s->relid, &r, &found);
		if (rc != PROLLY_OK)
			break;
		s->before = found;
		if (found)
			memcpy(s->old_n, r.nblocks, sizeof(s->old_n));
		if (s->edit == NULL)
		{
			s->after = found;
			memcpy(s->new_n, s->old_n, sizeof(s->new_n));
		}
		else if (!s->edit->del)
		{
			if (!rel_decode(s->relid, b->arena.data + s->edit->off, s->edit->len, &r))
				rc = fail(b->ctx, PROLLY_ERR_CORRUPT, NULL);
			s->after = true;
			memcpy(s->new_n, r.nblocks, sizeof(s->new_n));
		}
	}

	/* every given page must fall inside its relation's final blocks */
	npe_given = b->npe;
	for (size_t i = 0; i < npe_given && rc == PROLLY_OK; i++)
	{
		const PageEdit *e = &b->pe[i];
		RelState   *s = find_state(st, n, get_be32(e->key));
		int			fork = e->key[4];
		uint32_t	block = get_be32(e->key + 5);

		if (!s->after || block >= s->new_n[fork])
			rc = fail(b->ctx, IMG_ERR_INVALID, &e->page);
	}
	/* a relation that grew must be given every new block */
	for (size_t i = 0; i < n && rc == PROLLY_OK; i++)
	{
		RelState   *s = &st[i];

		for (int f = 0; f < IMG_NFORKS && rc == PROLLY_OK; f++)
		{
			uint32_t	from = s->old_n[f],
						to = s->after ? s->new_n[f] : 0;
			uint8_t		k[PAGE_KEY_LEN];
			size_t		lo = 0,
						hi = npe_given;
			uint32_t	expect;

			if (to <= from)
				continue;
			/* given pages are sorted: walk the ones in [from, to) */
			page_key(k, s->relid, f, from);
			while (lo < hi)
			{
				size_t		mid = lo + (hi - lo) / 2;

				if (memcmp(b->pe[mid].key, k, PAGE_KEY_LEN) < 0)
					lo = mid + 1;
				else
					hi = mid;
			}
			expect = from;
			for (size_t j = lo; j < npe_given && expect < to; j++)
			{
				if (get_be32(b->pe[j].key) != s->relid || b->pe[j].key[4] != f)
					break;
				if (get_be32(b->pe[j].key + 5) == expect)
					expect++;
				else if (get_be32(b->pe[j].key + 5) > expect)
					break;
			}
			if (expect != to)
				rc = fail(b->ctx, IMG_ERR_INVALID, NULL);
		}
	}
	/* pages a relation no longer has are dropped */
	for (size_t i = 0; i < n && rc == PROLLY_OK; i++)
	{
		RelState   *s = &st[i];

		for (int f = 0; f < IMG_NFORKS; f++)
		{
			uint32_t	keep = s->after ? s->new_n[f] : 0;

			for (uint32_t blk = keep; blk < s->old_n[f]; blk++)
				add_page_edit(b, s->relid, f, blk, NULL, true);
		}
	}
	if (rc != PROLLY_OK)
	{
		free(st);
		return rc;
	}
	*out = st;
	*nout = n;
	return PROLLY_OK;
}

int
img_finish(ImgBuilder *b, const PHash *commit, uint64_t lsn,
		   const ImgSnapshot *snap, PHash *out)
{
	ProllyCtx  *ctx = b->ctx;
	RelState   *st = NULL;
	size_t		nst = 0;
	ProllyEdit *pe = NULL,
			   *re = NULL;
	PHash		pages,
				rels;
	Buf			chunk = BUF_INIT;
	int			rc;

	rc = resolve(b, &st, &nst);
	if (rc != PROLLY_OK)
		goto done;

	pe = px_malloc(b->npe * sizeof(ProllyEdit));
	for (size_t i = 0; i < b->npe; i++)
		pe[i] = (ProllyEdit) {b->pe[i].key, PAGE_KEY_LEN, b->pe[i].page.b,
			b->pe[i].del ? 0 : PHASH_LEN, b->pe[i].del};
	re = px_malloc(b->nre * sizeof(ProllyEdit));
	{
		uint8_t    *keys = px_malloc(b->nre * REL_KEY_LEN + 1);

		for (size_t i = 0; i < b->nre; i++)
		{
			put_be32(keys + i * REL_KEY_LEN, b->re[i].relid);
			re[i] = (ProllyEdit) {keys + i * REL_KEY_LEN, REL_KEY_LEN,
				b->arena.data + b->re[i].off, b->re[i].len, b->re[i].del};
		}
		if (b->has_base)
		{
			pages = b->base.pages;
			rels = b->base.rels;
		}
		else if ((rc = prolly_empty(ctx, 1, &pages)) != PROLLY_OK ||
				 (rc = prolly_empty(ctx, 0, &rels)) != PROLLY_OK)
		{
			free(keys);
			goto done;
		}
		if ((rc = prolly_apply(ctx, &pages, pe, b->npe, &pages)) == PROLLY_OK)
			rc = prolly_apply(ctx, &rels, re, b->nre, &rels);
		free(keys);
		if (rc != PROLLY_OK)
			goto done;
	}

	buf_append_byte(&chunk, CHUNK_TAG_IMAGE);
	buf_append_byte(&chunk, CHUNK_FORMAT_VERSION);
	buf_append_uvarint(&chunk, b->format.pg_major);
	buf_append_uvarint(&chunk, b->format.page_size);
	buf_append_uvarint(&chunk, b->format.catversion);
	for (int i = 7; i >= 0; i--)
		buf_append_byte(&chunk, (uint8_t) (b->format.system_id >> (8 * i)));
	buf_append(&chunk, commit->b, PHASH_LEN);
	buf_append_byte(&chunk, b->has_base);
	if (b->has_base)
		buf_append(&chunk, b->base.hash.b, PHASH_LEN);
	buf_append_uvarint(&chunk, lsn);
	buf_append_uvarint(&chunk, snap->xmin);
	buf_append_uvarint(&chunk, snap->xmax);
	buf_append_uvarint(&chunk, snap->nxip);
	for (size_t i = 0; i < snap->nxip; i++)
		buf_append_uvarint(&chunk, snap->xip[i]);
	buf_append(&chunk, pages.b, PHASH_LEN);
	buf_append(&chunk, rels.b, PHASH_LEN);
	rc = prolly_put_chunk(ctx, chunk.data, chunk.len, out);

done:
	buf_free(&chunk);
	free(pe);
	free(re);
	free(st);
	img_abort(b);
	return rc;
}

/* ---------------------------------------------------------------- reading */

int
img_relation(ProllyCtx *ctx, const ImgHeader *h, uint32_t relid, ImgRelation *out)
{
	uint8_t		k[REL_KEY_LEN];
	const uint8_t *v;
	size_t		vl;
	int			rc;

	put_be32(k, relid);
	rc = prolly_get(ctx, &h->rels, k, REL_KEY_LEN, &v, &vl);
	if (rc != PROLLY_OK)
		return rc;
	if (!rel_decode(relid, v, vl, out))
		return fail(ctx, PROLLY_ERR_CORRUPT, &h->rels);
	return PROLLY_OK;
}

int
img_page(ProllyCtx *ctx, const ImgHeader *h, uint32_t relid, int fork,
		 uint32_t block, const uint8_t **page)
{
	uint8_t		k[PAGE_KEY_LEN];
	const uint8_t *v,
			   *d;
	size_t		vl,
				len;
	PHash		ph;
	int			rc;

	page_key(k, relid, fork, block);
	rc = prolly_get(ctx, &h->pages, k, PAGE_KEY_LEN, &v, &vl);
	if (rc != PROLLY_OK)
		return rc;
	if (vl != PHASH_LEN)
		return fail(ctx, PROLLY_ERR_CORRUPT, &h->pages);
	memcpy(ph.b, v, PHASH_LEN);
	d = cs_get(ctx->cs, &ph, &len);
	if (d == NULL)
		return fail(ctx, PROLLY_ERR_MISSING, &ph);
	if (len != (size_t) h->format.page_size + 2 || d[0] != CHUNK_TAG_PAGE)
		return fail(ctx, PROLLY_ERR_CORRUPT, &ph);
	*page = d + 2;
	return PROLLY_OK;
}

int
img_relations(ProllyCtx *ctx, const ImgHeader *h, ImgRelFn fn, void *arg)
{
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;
	int			rc = prolly_iter_open(ctx, &h->rels, NULL, 0, &it);

	if (rc != PROLLY_OK)
		return rc;
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) == 1)
	{
		ImgRelation r;

		if (kl != REL_KEY_LEN || !rel_decode(get_be32(k), v, vl, &r))
		{
			rc = fail(ctx, PROLLY_ERR_CORRUPT, &h->rels);
			break;
		}
		fn(arg, &r);
	}
	prolly_iter_close(it);
	return rc < 0 ? rc : PROLLY_OK;
}

typedef struct DiffArg
{
	ImgPageDiffFn fn;
	void	   *arg;
} DiffArg;

static void
page_diff_fn(void *arg, const ProllyDiff *d)
{
	DiffArg    *a = arg;
	PHash		from,
				to;

	if (d->from)
		memcpy(from.b, d->from, PHASH_LEN);
	if (d->to)
		memcpy(to.b, d->to, PHASH_LEN);
	a->fn(a->arg, get_be32(d->key), d->key[4], get_be32(d->key + 5),
		  d->from ? &from : NULL, d->to ? &to : NULL);
}

int
img_diff(ProllyCtx *ctx, const ImgHeader *from, const ImgHeader *to,
		 ImgPageDiffFn fn, void *arg)
{
	DiffArg		a = {fn, arg};

	return prolly_diff(ctx, &from->pages, &to->pages, page_diff_fn, &a);
}

typedef struct CheckRels
{
	ImgRelation *v;
	size_t		n;
	size_t		cap;
} CheckRels;

static void
collect_rel_fn(void *arg, const ImgRelation *r)
{
	CheckRels  *c = arg;

	px_grow((void **) &c->v, &c->cap, c->n + 1, sizeof(ImgRelation));
	c->v[c->n++] = *r;
}

int
img_check(ProllyCtx *ctx, const ImgHeader *h)
{
	CheckRels	rels = {0};
	uint32_t   *next;
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl,
				r = 0;
	int			rc = img_relations(ctx, h, collect_rel_fn, &rels);

	if (rc != PROLLY_OK)
		return rc;
	next = calloc(rels.n * IMG_NFORKS + 1, sizeof(uint32_t));
	rc = prolly_iter_open(ctx, &h->pages, NULL, 0, &it);
	if (rc == PROLLY_OK)
	{
		while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) == 1)
		{
			uint32_t	relid = get_be32(k);
			int			fork = k[4];
			uint32_t	block = get_be32(k + 5);
			const uint8_t *page;

			while (r < rels.n && rels.v[r].relid < relid)
				r++;
			if (kl != PAGE_KEY_LEN || r == rels.n || rels.v[r].relid != relid ||
				fork >= IMG_NFORKS || block != next[r * IMG_NFORKS + fork] ||
				block >= rels.v[r].nblocks[fork])
			{
				rc = fail(ctx, IMG_ERR_INVALID, NULL);
				break;
			}
			next[r * IMG_NFORKS + fork]++;
			if ((rc = img_page(ctx, h, relid, fork, block, &page)) != PROLLY_OK)
				break;
		}
		prolly_iter_close(it);
	}
	if (rc >= 0)
	{
		rc = PROLLY_OK;
		for (size_t i = 0; i < rels.n && rc == PROLLY_OK; i++)
			for (int f = 0; f < IMG_NFORKS; f++)
				if (next[i * IMG_NFORKS + f] != rels.v[i].nblocks[f])
					rc = fail(ctx, IMG_ERR_INVALID, NULL);
	}
	free(next);
	free(rels.v);
	return rc;
}
