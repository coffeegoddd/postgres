/*
 * graphbench.c
 *	  Disk used with parents in the commits (vc) against parents in a
 *	  commit graph (vcg), for the same stream of commits.
 *
 * A table of --rows rows; --commits commits, each of --changes random row
 * updates on one of --branches branches; the window keeps --window
 * generations (0: never sever) and severed history is discarded.  Every
 * commit saves the repo and flushes; every --compact-every commits the
 * store compacts.  Both engines get the same edits from the same seed, so
 * their table data is the same chunks, and what differs is history:
 * commits, the commit graph, segment records and the saved repo state.
 *
 * Prints disk and live bytes for both every --every commits, then the live
 * store by kind of chunk, and the bytes ever written.
 */
#define _DEFAULT_SOURCE
#include <getopt.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <time.h>

#include "../test/test_util.h"
#include "../vcg.h"

typedef struct Cfg
{
	uint64_t	rows;
	int			commits,
				changes,
				branches,
				window,
				compact_every,
				every;
} Cfg;

typedef struct Kind
{
	uint64_t	n,
				bytes;
} Kind;

typedef struct Breakdown
{
	Kind		data,			/* table trees, schemas, root values */
				commits,
				graph,			/* commit graph nodes */
				state,			/* saved repo state */
				other;
} Breakdown;

typedef struct Engine
{
	const char *name;
	ChunkStore *cs;
	bool		graph;
	VcRepo		vc;
	VcgRepo		vcg;
	uint64_t	disk[64],
				live[64];
	int			npoints;
	Breakdown	b;
	uint64_t	written;
	double		secs;
} Engine;

static const char *ddl = "CREATE TABLE t (id int8 PRIMARY KEY, v int8)";

/* -------------------------------------------------------------- engines */

static ProllyCtx *
ctx_of(Engine *e)
{
	return e->graph ? &e->vcg.ctx : &e->vc.ctx;
}

static PHash
head_root(Engine *e)
{
	if (e->graph)
	{
		VcgCommit	c;

		CHECK(vcg_commit_load(&e->vcg.ctx, &vcg_head(&e->vcg)->head, &c));
		return c.root;
	}
	return vc_working(&e->vc);
}

static void
commit(Engine *e, const PHash *rv, const char *msg)
{
	if (e->graph)
		vcg_commit(&e->vcg, rv, msg);
	else
	{
		vc_set_working(&e->vc, rv);
		vc_commit(&e->vc, msg);
	}
}

static void
checkout(Engine *e, const char *name)
{
	if (e->graph)
		CHECK(vcg_checkout(&e->vcg, name) == VCG_OK);
	else
		CHECK(vc_checkout(&e->vc, name) == VC_OK);
}

static void
branch(Engine *e, const char *name)
{
	if (e->graph)
		CHECK(vcg_branch_create(&e->vcg, name, &vcg_head(&e->vcg)->head) == VCG_OK);
	else
		CHECK(vc_branch_create(&e->vc, name, &vc_head(&e->vc)->head) == VC_OK);
}

static void
sever(Engine *e, int window)
{
	uint32_t	seg;

	if (window == 0)
		return;
	if (e->graph)
	{
		CHECK(vcg_sever(&e->vcg, (uint32_t) window, &seg) == VCG_OK);
		if (seg != VCG_NO_SEGMENT)
			CHECK(vcg_segment_discard(&e->vcg, seg) == VCG_OK);
	}
	else
	{
		CHECK(vc_sever(&e->vc, &(VcWindow) {(uint32_t) window, NULL, 0}, &seg) == VC_OK);
		if (seg != VC_NO_SEGMENT)
			CHECK(vc_segment_discard(&e->vc, seg) == VC_OK);
	}
}

static void
save(Engine *e)
{
	PHash		st = e->graph ? vcg_repo_save(&e->vcg) : vc_repo_save(&e->vc);

	dirstore_set_state(e->cs, &st);
	CHECK_OK(dirstore_flush(e->cs));
}

/* ------------------------------------------------------------ breakdown */

typedef struct Classify
{
	Engine	   *e;
	PHashMap	graph;
} Classify;

static void
add(Kind *k, size_t len)
{
	k->n++;
	k->bytes += len;
}

static void
classify_fn(void *arg, const PHash *h)
{
	Classify   *c = arg;
	size_t		len;
	const uint8_t *d = cs_get(c->e->cs, h, &len);

	if (d == NULL || len < 1)
		return;
	switch (d[0])
	{
		case CHUNK_TAG_COMMIT:
		case CHUNK_TAG_GCOMMIT:
			add(&c->e->b.commits, len);
			break;
		case CHUNK_TAG_REPO:
		case CHUNK_TAG_GREPO:
			add(&c->e->b.state, len);
			break;
		case CHUNK_TAG_NODE:
			if (phmap_find(&c->graph, h))
				add(&c->e->b.graph, len);
			else
				add(&c->e->b.data, len);
			break;
		case CHUNK_TAG_SCHEMA:
			add(&c->e->b.data, len);
			break;
		default:
			add(&c->e->b.other, len);
	}
}

static void
breakdown(Engine *e)
{
	Classify	c = {e, {0}};

	phmap_init(&c.graph);
	if (e->graph)
	{
		PHash		g = e->vcg.graph->ops->root(e->vcg.graph);

		closure(e->cs, &g, &c.graph);
	}
	memset(&e->b, 0, sizeof(e->b));
	cs_collect_orphans(e->cs);
	e->cs->ops->foreach(e->cs, classify_fn, &c);
	phmap_free(&c.graph);
}

/* ------------------------------------------------------------------ run */

static void
run(Engine *e, const Cfg *cfg, const char *dir)
{
	char		err[256];
	PHash		rv,
				data;
	double		t0 = now_sec();
	char		names[64][16];

	mkdir(dir, 0700);
	e->cs = dirstore_open(dir, err, sizeof(err));
	CHECK(e->cs != NULL);
	CHECK(e->cs->ops->count(e->cs) == 0);	/* an empty directory */
	rng_seed(99);
	if (e->graph)
	{
		vcg_repo_init(&e->vcg, e->cs, &prolly_default_policy);
		rv = vc_root_empty(&e->vcg.ctx);
	}
	else
	{
		vc_repo_init(&e->vc, e->cs, &prolly_default_policy);
		rv = vc_working(&e->vc);
	}
	CHECK(vc_table_create(ctx_of(e), &rv, "t", ddl, strlen(ddl), &rv) == 0);
	data = build_even_rows(ctx_of(e), cfg->rows);
	CHECK(vc_table_set_data(ctx_of(e), &rv, "t", &data, &rv) == 0);
	commit(e, &rv, "load");
	prolly_release_pins(ctx_of(e));
	for (int b = 1; b < cfg->branches && b < 64; b++)
	{
		snprintf(names[b], sizeof(names[b]), "b%d", b);
		branch(e, names[b]);
	}
	snprintf(names[0], sizeof(names[0]), "main");
	if (!e->graph && cfg->window)
		CHECK(vc_window_track(&e->vc, (uint32_t) cfg->window) == VC_OK);

	for (int i = 1; i <= cfg->commits; i++)
	{
		EditBatch	b;
		PHash		w,
					o;

		if (cfg->branches > 1)
			checkout(e, names[rnd_below((uint64_t) cfg->branches)]);
		w = head_root(e);
		batch_init(&b, (size_t) cfg->changes);
		for (int j = 0; j < cfg->changes; j++)
			batch_put(&b, rnd_below(cfg->rows) * 2, (uint64_t) i);
		CHECK(vc_table_edit(ctx_of(e), &w, "t", b.e, b.n, &o) == 0);
		batch_free(&b);
		commit(e, &o, "step");
		prolly_release_pins(ctx_of(e));
		sever(e, cfg->window);
		save(e);
		if (cfg->compact_every > 0 && i % cfg->compact_every == 0)
		{
			DirCompactStats s;

			CHECK_OK(dirstore_compact(e->cs, 1.0 / 3, &s));
		}
		if (i % cfg->every == 0 && e->npoints < 64)
		{
			e->disk[e->npoints] = dirstore_disk_bytes(e->cs);
			e->live[e->npoints++] = e->cs->ops->bytes(e->cs);
		}
	}
	e->secs = now_sec() - t0;
	e->written = e->cs->stats.new_bytes;
	breakdown(e);
	{
		PHash		bad;

		CHECK(dirstore_verify(e->cs, &bad));
	}
	if (e->graph)
		vcg_repo_free(&e->vcg);
	else
		vc_repo_free(&e->vc);
	cs_destroy(e->cs);
}

static void
kind_row(const char *what, const Kind *a, const Kind *b)
{
	printf("  %-22s %10" PRIu64 " %12.1f   %10" PRIu64 " %12.1f   %+12.1f\n", what,
		   a->n, a->bytes / 1024.0, b->n, b->bytes / 1024.0,
		   ((double) b->bytes - (double) a->bytes) / 1024.0);
}

int
main(int argc, char **argv)
{
	Cfg			cfg = {100000, 200, 100, 1, 20, 50, 20};
	const char *base = "/home/dustin/pgv2-bench/graphbench";
	static struct option opts[] = {
		{"rows", 1, 0, 'r'}, {"commits", 1, 0, 'c'}, {"changes", 1, 0, 'n'},
		{"branches", 1, 0, 'b'}, {"window", 1, 0, 'w'}, {"compact-every", 1, 0, 'k'},
		{"every", 1, 0, 'e'}, {"dir", 1, 0, 'd'}, {0, 0, 0, 0}
	};
	int			ch;
	char		dir[600];
	Engine		e[2] = {{.name = "vc"}, {.name = "vcg", .graph = true}};
	long		stamp = (long) time(NULL);

	while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1)
		switch (ch)
		{
			case 'r': cfg.rows = strtoull(optarg, NULL, 10); break;
			case 'c': cfg.commits = atoi(optarg); break;
			case 'n': cfg.changes = atoi(optarg); break;
			case 'b': cfg.branches = atoi(optarg); break;
			case 'w': cfg.window = atoi(optarg); break;
			case 'k': cfg.compact_every = atoi(optarg); break;
			case 'e': cfg.every = atoi(optarg); break;
			case 'd': base = optarg; break;
			default:
				fprintf(stderr, "graphbench [--rows N] [--commits N] [--changes N] [--branches B]\n"
						"           [--window W (0: never sever)] [--compact-every K] [--every N] [--dir D]\n");
				return 2;
		}
	mkdir(base, 0700);
	for (int i = 0; i < 2; i++)
	{
		snprintf(dir, sizeof(dir), "%s/%ld-%s", base, stamp, e[i].name);
		run(&e[i], &cfg, dir);
	}

	printf("# %" PRIu64 " rows, %d commits of %d changes on %d branch(es), window %d, compact every %d\n",
		   cfg.rows, cfg.commits, cfg.changes, cfg.branches, cfg.window, cfg.compact_every);
	printf("%8s %12s %12s   %12s %12s\n", "commits", "vc disk_mb", "vc live_mb",
		   "vcg disk_mb", "vcg live_mb");
	for (int p = 0; p < e[0].npoints; p++)
		printf("%8d %12.2f %12.3f   %12.2f %12.3f\n", (p + 1) * cfg.every,
			   e[0].disk[p] / 1048576.0, e[0].live[p] / 1048576.0,
			   e[1].disk[p] / 1048576.0, e[1].live[p] / 1048576.0);
	printf("\n  live at the end        %10s %12s   %10s %12s   %12s\n",
		   "vc chunks", "vc KB", "vcg chunks", "vcg KB", "vcg - vc KB");
	kind_row("table data", &e[0].b.data, &e[1].b.data);
	kind_row("commits", &e[0].b.commits, &e[1].b.commits);
	kind_row("commit graph", &e[0].b.graph, &e[1].b.graph);
	kind_row("repo state", &e[0].b.state, &e[1].b.state);
	kind_row("other", &e[0].b.other, &e[1].b.other);
	{
		Kind		ha = {e[0].b.commits.n + e[0].b.graph.n + e[0].b.state.n,
						  e[0].b.commits.bytes + e[0].b.graph.bytes + e[0].b.state.bytes},
					hb = {e[1].b.commits.n + e[1].b.graph.n + e[1].b.state.n,
						  e[1].b.commits.bytes + e[1].b.graph.bytes + e[1].b.state.bytes};

		kind_row("history, all", &ha, &hb);
	}
	printf("  written over the run   %23.1f MB %25.1f MB\n", e[0].written / 1048576.0,
		   e[1].written / 1048576.0);
	printf("  time                   %23.1f s  %25.1f s\n", e[0].secs, e[1].secs);
	return 0;
}
