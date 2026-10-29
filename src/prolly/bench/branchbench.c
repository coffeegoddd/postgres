/*
 * branchbench.c
 *	  How version control within the window scales with the number of
 *	  branches, at the engine level (no server).
 *
 * Each new branch starts at a random existing branch head and gets one
 * commit (a tenth get three): a few row edits in the prolly tree and a
 * commit image built on its parent's image with a few changed pages.  The
 * window is tracked incrementally and severed after every commit.  At each
 * milestone the per-operation costs over the last interval are printed, so
 * they can be compared as the branch count grows by orders of magnitude:
 * create, commit, image, sever, and switching between two random heads
 * (prolly row diff plus image page diff).
 */
#define _DEFAULT_SOURCE
#include <getopt.h>
#include <sys/resource.h>

#include "../test/test_util.h"
#include "../image.h"

#define PAGE 512				/* synthetic pages: small keeps a million images cheap */
#define REL 16384

typedef struct Ops
{
	double		create_us,
				commit_us,
				image_us,
				sever_us,
				switch_us;
	uint64_t	creates,
				commits,
				switches,
				switch_rows,
				switch_pages;
} Ops;

static ImgFormat fmt = {20, PAGE, 1, 42};
static const ImgSnapshot snap = {0, 0, NULL, 0};

static uint64_t rows_changed,
			pages_changed;

static void
row_edits(VcRepo *r, uint64_t nrows, int k, uint64_t ver)
{
	EditBatch	b;
	PHash		rv = vc_working(r),
				out;

	batch_init(&b, (size_t) k);
	for (int i = 0; i < k; i++)
		batch_put(&b, rnd_below(nrows), ver);
	CHECK(vc_table_edit(&r->ctx, &rv, "t", b.e, b.n, &out) == 0);
	vc_set_working(r, &out);
	batch_free(&b);
	rows_changed += (uint64_t) k;
}

static PHash
child_image(VcRepo *r, const PHash *parent_img, const PHash *commit, uint32_t npages,
			int k, uint32_t ver)
{
	ImgBuilder *b;
	ImgRelation rel;
	uint8_t		page[PAGE];
	PHash		img;

	memset(&rel, 0, sizeof(rel));
	rel.relid = REL;
	rel.kind = IMG_REL_TABLE;
	rel.name = "t";
	rel.namelen = 1;
	rel.nblocks[0] = npages;
	CHECK_OK(img_begin(&r->ctx, &fmt, parent_img, &b));
	if (parent_img == NULL)
	{
		CHECK_OK(img_put_relation(b, &rel));
		for (uint32_t blk = 0; blk < npages; blk++)
		{
			memset(page, 0, PAGE);
			put_u64(page, blk);
			CHECK_OK(img_put_page(b, REL, 0, blk, page));
		}
	}
	else
		for (int i = 0; i < k; i++)
		{
			uint32_t	blk = (uint32_t) rnd_below(npages);

			memset(page, 0, PAGE);
			put_u64(page, blk);
			put_u64(page + 8, ver);
			put_u64(page + 16, rnd());
			CHECK_OK(img_put_page(b, REL, 0, blk, page));
			pages_changed++;
		}
	CHECK_OK(img_finish(b, commit, 0, &snap, &img));
	return img;
}

typedef struct DiffCount
{
	uint64_t	n;
} DiffCount;

static void
row_count_fn(void *arg, const ProllyDiff *d)
{
	((DiffCount *) arg)->n++;
}

static void
page_count_fn(void *arg, uint32_t relid, int fork, uint32_t block,
			  const PHash *from, const PHash *to)
{
	((DiffCount *) arg)->n++;
}

static PHash
table_of(VcRepo *r, const PHash *commit)
{
	VcCommit	c;
	VcTable		t;

	CHECK(vc_commit_load(&r->ctx, commit, &c));
	CHECK(vc_table_get(&r->ctx, &c.root, "t", &t));
	return t.data;
}

static long
maxrss_mb(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_maxrss / 1024;
}

int
main(int argc, char **argv)
{
	uint64_t	max_branches = 1000000,
				nrows = 100000;
	uint32_t	npages = 20000,
				depth = 3;
	int			rows_per_commit = 3,
				pages_per_commit = 4;
	const char *store = "/home/dustin/pgv2-bench/branchbench.dat";
	static struct option opts[] = {
		{"branches", 1, 0, 'b'}, {"rows", 1, 0, 'r'}, {"pages", 1, 0, 'p'},
		{"depth", 1, 0, 'd'}, {"store", 1, 0, 'F'}, {"memory", 0, 0, 'm'}, {0, 0, 0, 0}
	};
	bool		memory = false;
	int			ch;
	ChunkStore *cs;
	VcRepo		r;
	char	  **names;
	PHash		c0,
				img0,
				rv;
	Ops			ops;
	uint64_t	next_milestone = 1000;
	double		start = now_sec();
	ChunkPolicy policy = {256, 1024, 4096};

	while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1)
		switch (ch)
		{
			case 'b': max_branches = strtoull(optarg, NULL, 10); break;
			case 'r': nrows = strtoull(optarg, NULL, 10); break;
			case 'p': npages = (uint32_t) atoi(optarg); break;
			case 'd': depth = (uint32_t) atoi(optarg); break;
			case 'F': store = optarg; break;
			case 'm': memory = true; break;
			default:
				fprintf(stderr, "branchbench [--branches N] [--rows N] [--pages N] [--depth D] [--store F|--memory]\n");
				return 2;
		}
	rng_seed(7);
	cs = memory ? memstore_create() : filestore_create(store);
	vc_repo_init(&r, cs, &policy);
	rv = vc_working(&r);
	CHECK(vc_table_create(&r.ctx, &rv, "t", "t", 1, &rv) == 0);
	{
		PHash		data = build_even_rows(&r.ctx, nrows);

		CHECK(vc_table_set_data(&r.ctx, &rv, "t", &data, &rv) == 0);
	}
	vc_set_working(&r, &rv);
	c0 = vc_commit(&r, "base");
	img0 = child_image(&r, NULL, &c0, npages, 0, 0);
	CHECK(vc_image_set(&r, &c0, &img0) == VC_OK);
	CHECK(vc_window_track(&r, depth) == VC_OK);
	names = px_malloc((max_branches + 1) * sizeof(char *));
	names[0] = px_memdup("main", 5);
	memset(&ops, 0, sizeof(ops));

	printf("%10s %9s %9s %9s %9s %10s %12s %10s %10s %8s %8s\n",
		   "branches", "create_us", "commit_us", "image_us", "sever_us", "switch_us",
		   "switch_diff", "store_mb", "window", "rss_mb", "elapsed");
	for (uint64_t b = 1; b <= max_branches; b++)
	{
		char		name[24];
		VcBranch   *parent = vc_branch_find(&r, names[rnd_below(b)]);
		PHash		phead = parent->head,
					pimg;
		double		t;
		int			ncommits = rnd_below(10) == 0 ? 3 : 1;

		snprintf(name, sizeof(name), "b%llu", (unsigned long long) b);
		names[b] = px_memdup(name, strlen(name) + 1);
		t = now_sec();
		CHECK(vc_branch_create(&r, name, &phead) == VC_OK);
		CHECK(vc_checkout(&r, name) == VC_OK);
		ops.create_us += (now_sec() - t) * 1e6;
		ops.creates++;
		CHECK(vc_image_get(&r, &phead, &pimg));

		for (int c = 0; c < ncommits; c++)
		{
			PHash		commit,
						img;
			uint32_t	seg;

			t = now_sec();
			row_edits(&r, nrows, rows_per_commit, b * 4 + (uint64_t) c);
			commit = vc_commit(&r, "branch work");
			ops.commit_us += (now_sec() - t) * 1e6;
			t = now_sec();
			img = child_image(&r, &pimg, &commit, npages, pages_per_commit, (uint32_t) b);
			CHECK(vc_image_set(&r, &commit, &img) == VC_OK);
			ops.image_us += (now_sec() - t) * 1e6;
			ops.commits++;
			pimg = img;
			t = now_sec();
			CHECK(vc_sever(&r, &(VcWindow) {.depth = depth}, &seg) == VC_OK);
			if (seg != VC_NO_SEGMENT)
				CHECK(vc_segment_discard(&r, seg) == VC_OK);
			ops.sever_us += (now_sec() - t) * 1e6;
		}

		/* every 16th branch: switch between two random heads */
		if (b % 16 == 0)
		{
			VcBranch   *x = vc_branch_find(&r, names[rnd_below(b + 1)]);
			VcBranch   *y = vc_branch_find(&r, names[rnd_below(b + 1)]);
			PHash		xi,
						yi,
						xt,
						yt;
			ImgHeader	hx,
						hy;
			DiffCount	rows = {0},
						pages = {0};

			t = now_sec();
			CHECK(vc_checkout(&r, x->name) == VC_OK);
			xt = table_of(&r, &x->head);
			yt = table_of(&r, &y->head);
			CHECK_OK(prolly_diff(&r.ctx, &xt, &yt, row_count_fn, &rows));
			CHECK(vc_image_get(&r, &x->head, &xi) && vc_image_get(&r, &y->head, &yi));
			CHECK_OK(img_load(&r.ctx, &xi, &hx));
			CHECK_OK(img_load(&r.ctx, &yi, &hy));
			CHECK_OK(img_diff(&r.ctx, &hx, &hy, page_count_fn, &pages));
			img_header_free(&hx);
			img_header_free(&hy);
			ops.switch_us += (now_sec() - t) * 1e6;
			ops.switches++;
			ops.switch_rows += rows.n;
			ops.switch_pages += pages.n;
		}

		if (b == next_milestone || b == max_branches)
		{
			size_t		window = 0;

			for (size_t i = 0; i < r.loc.cap; i++)
				if (r.loc.used[i] && r.loc.vals[i] == 1)
					window++;
			printf("%10llu %9.1f %9.1f %9.1f %9.1f %10.1f %6.1fr/%-5.1fp %10.0f %10zu %8ld %7.0fs\n",
				   (unsigned long long) b,
				   ops.create_us / (double) ops.creates,
				   ops.commit_us / (double) ops.commits,
				   ops.image_us / (double) ops.commits,
				   ops.sever_us / (double) ops.commits,
				   ops.switches ? ops.switch_us / (double) ops.switches : 0,
				   ops.switches ? (double) ops.switch_rows / (double) ops.switches : 0,
				   ops.switches ? (double) ops.switch_pages / (double) ops.switches : 0,
				   cs->ops->bytes(cs) / 1048576.0, window, maxrss_mb(), now_sec() - start);
			fflush(stdout);
			memset(&ops, 0, sizeof(ops));
			next_milestone *= 10;
		}
	}
	CHECK(vc_window_verify(&r));
	printf("# %llu rows changed, %llu pages changed; window verified\n",
		   (unsigned long long) rows_changed, (unsigned long long) pages_changed);
	for (uint64_t b = 0; b <= max_branches; b++)
		free(names[b]);
	free(names);
	vc_repo_free(&r);
	cs_destroy(cs);
	return 0;
}
