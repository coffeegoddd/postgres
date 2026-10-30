/*
 * test_image.c
 *	  Commit images: building from scratch and on a base, reading pages and
 *	  relations back, refusing images with holes or stray pages, sharing
 *	  identical pages, diffing images, the window's holds on them, errors,
 *	  and costs.
 */
#include "test_repo.h"
#include "image.h"

#define PAGE 8192

static const ImgFormat fmt = {19, PAGE, 202610281, 0x5eed5eed5eedULL};

#define T_REL 16384				/* a table */
#define I_REL 16390				/* its primary key index */
#define X_REL 16388				/* its TOAST table */
#define N_REL 16400				/* a table added later */

/* A page whose bytes are a function of where it is and its version. */
static void
fill_page(uint8_t *p, size_t size, uint32_t relid, int fork, uint32_t block,
		  uint32_t version)
{
	uint64_t	x = ((uint64_t) relid << 40) ^ ((uint64_t) fork << 32) ^
		((uint64_t) block << 8) ^ version;

	for (size_t i = 0; i < size; i += 8)
	{
		x = x * 6364136223846793005ULL + 1442695040888963407ULL;
		memcpy(p + i, &x, 8);
	}
}

static ImgRelation
rel(uint32_t relid, ImgRelKind kind, uint32_t parent, const char *name,
	uint32_t main, uint32_t fsm, uint32_t vm)
{
	ImgRelation r;

	memset(&r, 0, sizeof(r));
	r.relid = relid;
	r.kind = kind;
	r.parent = parent;
	r.name = name;
	r.namelen = strlen(name);
	r.nblocks[0] = main;
	r.nblocks[1] = fsm;
	r.nblocks[2] = vm;
	r.catalog = (const uint8_t *) name;	/* stands in for catalog rows */
	r.catlen = strlen(name);
	return r;
}

static void
put_rel_pages(ImgBuilder *b, const ImgRelation *r, uint32_t version)
{
	uint8_t		page[PAGE];

	for (int f = 0; f < IMG_NFORKS; f++)
		for (uint32_t blk = 0; blk < r->nblocks[f]; blk++)
		{
			fill_page(page, PAGE, r->relid, f, blk, version);
			CHECK_OK(img_put_page(b, r->relid, f, blk, page));
		}
}

static const PHash no_commit = {{1}};
static uint64_t xips[] = {1007, 1009};
static const ImgSnapshot isnap = {1000, 1010, xips, 2};

/* table t (20+3+1 blocks), its index (8), its TOAST table (5) */
static PHash
build_base(ProllyCtx *ctx)
{
	ImgBuilder *b;
	ImgRelation t = rel(T_REL, IMG_REL_TABLE, 0, "t", 20, 3, 1);
	ImgRelation i = rel(I_REL, IMG_REL_INDEX, T_REL, "t_pkey", 8, 0, 0);
	ImgRelation x = rel(X_REL, IMG_REL_TOAST, T_REL, "pg_toast_16384", 5, 0, 0);
	PHash		h;

	CHECK_OK(img_begin(ctx, &fmt, NULL, &b));
	CHECK_OK(img_put_relation(b, &t));
	CHECK_OK(img_put_relation(b, &i));
	CHECK_OK(img_put_relation(b, &x));
	put_rel_pages(b, &t, 0);
	put_rel_pages(b, &i, 0);
	put_rel_pages(b, &x, 0);
	CHECK_OK(img_finish(b, &no_commit, 5000, &isnap, &h));
	return h;
}

static void
check_page(ProllyCtx *ctx, const ImgHeader *h, uint32_t relid, int fork,
		   uint32_t block, uint32_t version)
{
	uint8_t		want[PAGE];
	const uint8_t *got;

	fill_page(want, PAGE, relid, fork, block, version);
	CHECK_OK(img_page(ctx, h, relid, fork, block, &got));
	CHECK(memcmp(got, want, PAGE) == 0);
}

typedef struct RelList
{
	uint32_t	ids[16];
	int			n;
} RelList;

static void
list_rel_fn(void *arg, const ImgRelation *r)
{
	RelList    *l = arg;

	l->ids[l->n++] = r->relid;
}

/* ------------------------------------------------------------------ tests */

static void
test_build_and_read(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		h;
	ImgHeader	hdr;
	ImgRelation r;
	RelList		l = {{0}, 0};

	prolly_ctx_init(&ctx, cs, &small_policy);
	h = build_base(&ctx);
	CHECK_OK(img_load(&ctx, &h, &hdr));
	CHECK(img_compatible(&hdr, &fmt));
	CHECK(phash_eq(&hdr.commit, &no_commit) && !hdr.has_base && hdr.lsn == 5000);
	CHECK(hdr.snapshot.xmin == 1000 && hdr.snapshot.xmax == 1010);
	CHECK(hdr.snapshot.nxip == 2 && hdr.snapshot.xip[1] == 1009);

	for (uint32_t b = 0; b < 20; b++)
		check_page(&ctx, &hdr, T_REL, 0, b, 0);
	check_page(&ctx, &hdr, T_REL, 1, 2, 0);
	check_page(&ctx, &hdr, T_REL, 2, 0, 0);
	check_page(&ctx, &hdr, I_REL, 0, 7, 0);
	check_page(&ctx, &hdr, X_REL, 0, 4, 0);
	{
		const uint8_t *p;

		CHECK(img_page(&ctx, &hdr, T_REL, 0, 20, &p) == PROLLY_NOT_FOUND);
		CHECK(img_page(&ctx, &hdr, T_REL, 3, 0, &p) == PROLLY_NOT_FOUND);
	}

	CHECK_OK(img_relation(&ctx, &hdr, I_REL, &r));
	CHECK(r.kind == IMG_REL_INDEX && r.parent == T_REL && r.nblocks[0] == 8);
	CHECK(r.namelen == 6 && memcmp(r.name, "t_pkey", 6) == 0);
	CHECK(r.catlen == 6 && memcmp(r.catalog, "t_pkey", 6) == 0);
	CHECK_OK(img_relations(&ctx, &hdr, list_rel_fn, &l));
	CHECK(l.n == 3 && l.ids[0] == T_REL && l.ids[1] == X_REL && l.ids[2] == I_REL);
	CHECK_OK(img_check(&ctx, &hdr));

	cs_ref_root(cs, &h);
	prolly_release_pins(&ctx);
	check_store(cs, 0);
	img_header_free(&hdr);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

typedef struct DiffTally
{
	int			changed,
				added,
				removed;
	int			by_rel[4];
} DiffTally;

static void
tally_fn(void *arg, uint32_t relid, int fork, uint32_t block,
		 const PHash *from, const PHash *to)
{
	DiffTally  *t = arg;

	if (from && to)
		t->changed++;
	else if (to)
		t->added++;
	else
		t->removed++;
	t->by_rel[relid == T_REL ? 0 : relid == I_REL ? 1 : relid == X_REL ? 2 : 3]++;
}

/*
 * On a base: rewrite 5 table pages, grow the table by 3 blocks, shrink the
 * index by 2, drop the TOAST table, add a table.
 */
static void
test_incremental_image(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		base,
				next;
	ImgBuilder *b;
	ImgHeader	hb,
				hn;
	ImgRelation t = rel(T_REL, IMG_REL_TABLE, 0, "t", 23, 3, 1);
	ImgRelation i = rel(I_REL, IMG_REL_INDEX, T_REL, "t_pkey", 6, 0, 0);
	ImgRelation n = rel(N_REL, IMG_REL_TABLE, 0, "u", 4, 0, 0);
	uint8_t		page[PAGE];
	uint64_t	new0;
	DiffTally	d = {0};
	ImgRelation r;

	prolly_ctx_init(&ctx, cs, &small_policy);
	base = build_base(&ctx);
	new0 = cs->stats.new_chunks;
	CHECK_OK(img_begin(&ctx, &fmt, &base, &b));
	CHECK_OK(img_put_relation(b, &t));
	CHECK_OK(img_put_relation(b, &i));
	CHECK_OK(img_drop_relation(b, X_REL));
	CHECK_OK(img_put_relation(b, &n));
	for (uint32_t blk = 2; blk < 12; blk += 2)
	{
		fill_page(page, PAGE, T_REL, 0, blk, 1);
		CHECK_OK(img_put_page(b, T_REL, 0, blk, page));
	}
	for (uint32_t blk = 20; blk < 23; blk++)
	{
		fill_page(page, PAGE, T_REL, 0, blk, 1);
		CHECK_OK(img_put_page(b, T_REL, 0, blk, page));
	}
	put_rel_pages(b, &n, 1);
	CHECK_OK(img_finish(b, &no_commit, 6000, &isnap, &next));
	/* 12 new pages, plus the few map nodes and the image above them */
	CHECK_LE(cs->stats.new_chunks - new0, 12 + 40);

	CHECK_OK(img_load(&ctx, &base, &hb));
	CHECK_OK(img_load(&ctx, &next, &hn));
	CHECK(hn.has_base && phash_eq(&hn.base, &base));
	CHECK_OK(img_check(&ctx, &hn));
	check_page(&ctx, &hn, T_REL, 0, 4, 1);
	check_page(&ctx, &hn, T_REL, 0, 5, 0);	/* untouched, shared with base */
	check_page(&ctx, &hn, T_REL, 0, 22, 1);
	check_page(&ctx, &hn, N_REL, 0, 3, 1);
	check_page(&ctx, &hn, I_REL, 0, 5, 0);
	{
		const uint8_t *p;

		CHECK(img_page(&ctx, &hn, I_REL, 0, 6, &p) == PROLLY_NOT_FOUND);
		CHECK(img_page(&ctx, &hn, X_REL, 0, 0, &p) == PROLLY_NOT_FOUND);
		CHECK(img_relation(&ctx, &hn, X_REL, &r) == PROLLY_NOT_FOUND);
	}
	/* the base is untouched */
	CHECK_OK(img_check(&ctx, &hb));
	check_page(&ctx, &hb, X_REL, 0, 4, 0);

	CHECK_OK(img_diff(&ctx, &hb, &hn, tally_fn, &d));
	CHECK(d.changed == 5 && d.added == 3 + 4 && d.removed == 2 + 5);
	CHECK(d.by_rel[0] == 8 && d.by_rel[1] == 2 && d.by_rel[2] == 5 && d.by_rel[3] == 4);

	img_header_free(&hb);
	img_header_free(&hn);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* Each kind of bad image is refused, and leaves nothing behind. */
static void
test_builder_rejects_invalid(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		base,
				out;
	ImgBuilder *b;
	uint8_t		page[PAGE];
	ImgRelation t5 = rel(T_REL, IMG_REL_TABLE, 0, "t", 25, 3, 1);
	ImgRelation bad = rel(N_REL, (ImgRelKind) 'q', 0, "q", 1, 0, 0);
	ImgFormat	other = fmt;
	size_t		count;

	prolly_ctx_init(&ctx, cs, &small_policy);
	base = build_base(&ctx);
	cs_ref_root(cs, &base);
	prolly_release_pins(&ctx);
	count = cs->ops->count(cs);
	fill_page(page, PAGE, 1, 0, 0, 9);

	/* a page for a relation that does not exist */
	CHECK_OK(img_begin(&ctx, &fmt, &base, &b));
	CHECK_OK(img_put_page(b, N_REL, 0, 0, page));
	CHECK(img_finish(b, &no_commit, 1, &isnap, &out) == IMG_ERR_INVALID);
	/* past the end of its relation */
	CHECK_OK(img_begin(&ctx, &fmt, &base, &b));
	CHECK_OK(img_put_page(b, I_REL, 0, 8, page));
	CHECK(img_finish(b, &no_commit, 1, &isnap, &out) == IMG_ERR_INVALID);
	/* growth with a hole: blocks 20..24, 22 missing */
	CHECK_OK(img_begin(&ctx, &fmt, &base, &b));
	CHECK_OK(img_put_relation(b, &t5));
	for (uint32_t blk = 20; blk < 25; blk++)
		if (blk != 22)
			CHECK_OK(img_put_page(b, T_REL, 0, blk, page));
	CHECK(img_finish(b, &no_commit, 1, &isnap, &out) == IMG_ERR_INVALID);
	/* a page for a relation dropped in the same image */
	CHECK_OK(img_begin(&ctx, &fmt, &base, &b));
	CHECK_OK(img_drop_relation(b, X_REL));
	CHECK_OK(img_put_page(b, X_REL, 0, 0, page));
	CHECK(img_finish(b, &no_commit, 1, &isnap, &out) == IMG_ERR_INVALID);
	/* a new relation missing pages */
	CHECK_OK(img_begin(&ctx, &fmt, &base, &b));
	{
		ImgRelation n = rel(N_REL, IMG_REL_TABLE, 0, "u", 2, 0, 0);

		CHECK_OK(img_put_relation(b, &n));
	}
	CHECK_OK(img_put_page(b, N_REL, 0, 0, page));
	CHECK(img_finish(b, &no_commit, 1, &isnap, &out) == IMG_ERR_INVALID);
	/* bad kind, bad fork */
	CHECK_OK(img_begin(&ctx, &fmt, &base, &b));
	CHECK(img_put_relation(b, &bad) == IMG_ERR_INVALID);
	CHECK(img_put_page(b, T_REL, 4, 0, page) == IMG_ERR_INVALID);
	img_abort(b);
	/* a base built for another cluster */
	other.system_id++;
	CHECK(img_begin(&ctx, &other, &base, &b) == IMG_ERR_FORMAT);

	prolly_release_pins(&ctx);
	CHECK_EQ(cs->ops->count(cs), count);
	check_store(cs, 0);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* Identical pages, anywhere, are one chunk. */
static void
test_identical_pages_stored_once(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	ImgBuilder *b;
	ImgRelation t = rel(T_REL, IMG_REL_TABLE, 0, "t", 60, 0, 0);
	ImgRelation i = rel(I_REL, IMG_REL_INDEX, T_REL, "t_pkey", 40, 0, 0);
	uint8_t		zero[PAGE];
	PHash		h;
	size_t		pages = 0;

	prolly_ctx_init(&ctx, cs, &small_policy);
	memset(zero, 0, PAGE);
	CHECK_OK(img_begin(&ctx, &fmt, NULL, &b));
	CHECK_OK(img_put_relation(b, &t));
	CHECK_OK(img_put_relation(b, &i));
	for (uint32_t blk = 0; blk < 60; blk++)
		CHECK_OK(img_put_page(b, T_REL, 0, blk, zero));
	for (uint32_t blk = 0; blk < 40; blk++)
		CHECK_OK(img_put_page(b, I_REL, 0, blk, zero));
	CHECK_OK(img_finish(b, &no_commit, 1, &isnap, &h));
	{
		PHashMap	all;

		phmap_init(&all);
		closure(cs, &h, &all);
		for (size_t k = 0; k < all.cap; k++)
			if (all.used[k])
			{
				size_t		len;
				const uint8_t *d = cs->ops->get(cs, &all.keys[k], &len);

				pages += d[0] == CHUNK_TAG_PAGE;
			}
		phmap_free(&all);
	}
	CHECK_EQ(pages, 1);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* Images belong to the window: severing a commit frees exactly its image's own pages. */
static void
test_images_bundled_with_segments(void)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *acs = memstore_create();
	VcRepo		r;
	PHash		c[4],
				img[4],
				got;
	uint32_t	seg;
	size_t		want;
	ChunkStoreStats st;

	setup(&r, cs);
	for (int k = 0; k < 4; k++)
	{
		ImgBuilder *b;
		ImgRelation t = rel(T_REL, IMG_REL_TABLE, 0, "t", 20, 3, 1);
		uint8_t		page[PAGE];

		c[k] = commit_rows(&r, (uint64_t) k * 100, (uint64_t) k * 100 + 50,
						   (uint64_t) k, "commit");
		if (k == 0)
		{
			img[k] = build_base(&r.ctx);
		}
		else
		{
			CHECK_OK(img_begin(&r.ctx, &fmt, &img[k - 1], &b));
			CHECK_OK(img_put_relation(b, &t));
			for (uint32_t blk = 0; blk < 4; blk++)
			{
				fill_page(page, PAGE, T_REL, 0, blk + 4 * (uint32_t) k, (uint32_t) k);
				CHECK_OK(img_put_page(b, T_REL, 0, blk + 4 * (uint32_t) k, page));
			}
			CHECK_OK(img_finish(b, &c[k], 100 + (uint64_t) k, &isnap, &img[k]));
		}
		CHECK(vc_image_set(&r, &c[k], &img[k]) == VC_OK);
	}
	check_store(cs, 0);

	/* the chunks only the images of c0 and c1 reach */
	{
		PHashMap	mine,
					other,
					only;

		phmap_init(&mine);
		closure(cs, &img[0], &mine);
		closure(cs, &img[1], &mine);
		held_elsewhere(&r, VC_NO_SEGMENT, &other);
		closure(cs, &img[2], &other);
		closure(cs, &img[3], &other);
		set_minus(&mine, &other, &only);
		want = set_size(&only);
		phmap_free(&mine);
		phmap_free(&other);
		phmap_free(&only);
	}
	CHECK(want > 0);

	/* severing moves c0's and c1's images into the segment, freeing nothing */
	st = cs->stats;
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK_EQ(STATS_DELTA(cs, st, frees), 0);
	CHECK(!vc_image_get(&r, &c[1], &got));
	CHECK(vc_image_get(&r, &c[2], &got) && phash_eq(&got, &img[2]));
	CHECK(vc_image_set(&r, &c[1], &img[2]) == VC_ERR_OUTSIDE_WINDOW);
	CHECK_EQ(r.segs[seg].nkf, 2);
	check_store(cs, 0);

	/*
	 * exporting the segment takes its keyframes with it: its manifest lists
	 * each with its commit, the archive holds their pages, and here what
	 * only they (and the segment's history) reached is freed
	 */
	st = cs->stats;
	CHECK(vc_segment_export(&r, seg, &(VcSource) {"cold", acs}) == VC_OK);
	CHECK(STATS_DELTA(cs, st, frees) >= want);	/* and their history with them */
	{
		PHash	   *mans;
		size_t		nm;
		VcManifest	m;
		PHashMap	all;
		size_t		pages = 0;

		vc_archive_list(acs, &mans, &nm);
		CHECK_EQ(nm, 1);
		CHECK(vc_manifest_load(acs, &mans[0], &m));
		CHECK_EQ(m.nkf, 2);
		for (size_t k = 0; k < m.nkf; k++)
		{
			ImgHeader	h;
			ProllyCtx	actx;

			prolly_ctx_init(&actx, acs, &small_policy);
			CHECK_OK(img_load(&actx, &m.kf_images[k], &h));
			/*
			 * a keyframe names its commit, the one the manifest pairs it
			 * with (c0's here is the fixture's base image, of no commit)
			 */
			CHECK(phash_eq(&m.kf_commits[k], &c[0]) || phash_eq(&m.kf_commits[k], &c[1]));
			if (phash_eq(&m.kf_commits[k], &c[1]))
				CHECK(phash_eq(&h.commit, &c[1]));
			img_header_free(&h);
			prolly_ctx_free(&actx);
		}
		vc_manifest_free(&m);
		phmap_init(&all);
		closure(acs, &mans[0], &all);
		for (size_t k = 0; k < all.cap; k++)
			if (all.used[k])
			{
				size_t		len;
				const uint8_t *d = acs->ops->get(acs, &all.keys[k], &len);

				pages += d[0] == CHUNK_TAG_PAGE;
			}
		CHECK(pages > 0);
		phmap_free(&all);
		free(mans);
	}
	check_store(cs, 0);

	/* attaching brings the history back and the keyframes with it */
	CHECK(vc_segment_attach(&r, seg) == VC_OK);
	CHECK(vc_image_get(&r, &c[0], &got) && phash_eq(&got, &img[0]));
	CHECK(vc_image_get(&r, &c[1], &got) && phash_eq(&got, &img[1]));
	check_store(cs, 0);

	/* replacing a commit's image frees what only the old one had */
	CHECK(vc_image_set(&r, &c[3], &img[2]) == VC_OK);
	check_store(cs, 0);
	CHECK(vc_image_get(&r, &c[3], &got) && phash_eq(&got, &img[2]));

	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(acs);
}

static void
test_image_errors(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		h,
				victim,
				commit = {{9}};
	ImgHeader	hdr;
	const uint8_t *p;
	ImgBuilder *b;

	prolly_ctx_init(&ctx, cs, &small_policy);
	h = build_base(&ctx);
	cs_ref_root(cs, &h);
	prolly_release_pins(&ctx);
	CHECK_OK(img_load(&ctx, &h, &hdr));
	{
		PHash		pages = hdr.pages;
		ImgHeader	other;

		CHECK(img_load(&ctx, &pages, &other) == PROLLY_ERR_CORRUPT);	/* not an image */
	}
	img_header_free(&hdr);
	CHECK(img_load(&ctx, &commit, &hdr) == PROLLY_ERR_MISSING);
	CHECK(img_begin(&ctx, &fmt, &commit, &b) == PROLLY_ERR_MISSING);
	CHECK_OK(img_load(&ctx, &h, &hdr));
	{
		uint8_t		want[PAGE];

		fill_page(want, PAGE, T_REL, 0, 3, 0);
		{
			Buf			c = BUF_INIT;

			buf_append_byte(&c, CHUNK_TAG_PAGE);
			buf_append_byte(&c, CHUNK_FORMAT_VERSION);
			buf_append(&c, want, PAGE);
			phash_compute(c.data, c.len, &victim);
			buf_free(&c);
		}
	}
	cs->ops->del(cs, &victim);	/* damage */
	CHECK(img_page(&ctx, &hdr, T_REL, 0, 3, &p) == PROLLY_ERR_MISSING);
	CHECK(phash_eq(&ctx.err_hash, &victim));
	CHECK(img_check(&ctx, &hdr) == PROLLY_ERR_MISSING);
	img_header_free(&hdr);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* ---------------------------------------------------------------- scaling */

#define SP 512					/* small pages keep big relations cheap to test */
#define SCALE_CHANGES 20

typedef struct ImgScale
{
	uint32_t	pages;
	uint64_t	new_chunks;
	uint64_t	diff_reads;
	uint64_t	read_reads;
} ImgScale;

static void
img_scale_case(uint32_t npages, ImgScale *s)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	ImgFormat	f = fmt;
	ImgBuilder *b;
	ImgRelation t = rel(T_REL, IMG_REL_TABLE, 0, "t", npages, 0, 0);
	uint8_t		page[SP];
	PHash		base,
				next;
	ImgHeader	hb,
				hn;
	ChunkStoreStats st;
	DiffTally	d = {0};
	const uint8_t *p;

	memset(s, 0, sizeof(*s));
	s->pages = npages;
	f.page_size = SP;
	rng_seed(npages);
	prolly_ctx_init(&ctx, cs, &prolly_default_policy);
	CHECK_OK(img_begin(&ctx, &f, NULL, &b));
	CHECK_OK(img_put_relation(b, &t));
	for (uint32_t blk = 0; blk < npages; blk++)
	{
		fill_page(page, SP, T_REL, 0, blk, 0);
		CHECK_OK(img_put_page(b, T_REL, 0, blk, page));
	}
	CHECK_OK(img_finish(b, &no_commit, 1, &isnap, &base));

	st = cs->stats;
	CHECK_OK(img_begin(&ctx, &f, &base, &b));
	for (int k = 0; k < SCALE_CHANGES; k++)
	{
		uint32_t	blk = (uint32_t) rnd_below(npages);

		fill_page(page, SP, T_REL, 0, blk, 1);
		CHECK_OK(img_put_page(b, T_REL, 0, blk, page));
	}
	CHECK_OK(img_finish(b, &no_commit, 2, &isnap, &next));
	s->new_chunks = STATS_DELTA(cs, st, new_chunks);

	CHECK_OK(img_load(&ctx, &base, &hb));
	CHECK_OK(img_load(&ctx, &next, &hn));
	st = cs->stats;
	CHECK_OK(img_diff(&ctx, &hb, &hn, tally_fn, &d));
	s->diff_reads = STATS_DELTA(cs, st, gets);
	CHECK(d.changed > 0 && d.changed <= SCALE_CHANGES && d.added + d.removed == 0);
	st = cs->stats;
	CHECK_OK(img_page(&ctx, &hn, T_REL, 0, npages / 2, &p));
	s->read_reads = STATS_DELTA(cs, st, gets);

	img_header_free(&hb);
	img_header_free(&hn);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

static void
test_image_scaling(void)
{
	ImgScale	s[2];

	img_scale_case(10000, &s[0]);
	img_scale_case(100000, &s[1]);
	printf("\n  %8s %14s %12s %12s\n", "pages", "next image new", "diff reads", "page read");
	for (int i = 0; i < 2; i++)
		printf("  %8u %14llu %12llu %12llu\n", s[i].pages,
			   (unsigned long long) s[i].new_chunks,
			   (unsigned long long) s[i].diff_reads,
			   (unsigned long long) s[i].read_reads);
	for (int i = 0; i < 2; i++)
	{
		/* 20 changed pages: the pages, a few map nodes per page, the image */
		CHECK_LE(s[i].new_chunks, SCALE_CHANGES * 5 + 5);
		CHECK_LE(s[i].diff_reads, SCALE_CHANGES * 4 * 4 + 4);
		CHECK_LE(s[i].read_reads, 6);
	}
	/* 10x the pages: costs follow the change, not the relation */
	CHECK_LE(s[1].new_chunks, 2 * s[0].new_chunks);
	CHECK_LE(s[1].diff_reads, 2 * s[0].diff_reads);
	printf("%-44s", "");
}

int
main(void)
{
	RUN(test_build_and_read);
	RUN(test_incremental_image);
	RUN(test_builder_rejects_invalid);
	RUN(test_identical_pages_stored_once);
	RUN(test_images_bundled_with_segments);
	RUN(test_image_errors);
	RUN(test_image_scaling);
	printf("test_image: all tests passed\n");
	return 0;
}
