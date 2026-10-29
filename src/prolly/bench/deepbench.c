/*
 * deepbench.c
 *	  Version control with millions of commits inside the window: one deep
 *	  history, on the durable store, at the engine level.
 *
 * A table of --rows rows; --commits commits of --changes random row
 * updates each (the size of a transaction), all on main, all kept: the
 * window is deeper than the history.  The repo is saved every --save-every
 * commits and the store flushed every --flush-every; the window is severed
 * every --sever-every commits (a full pass; with --due, whenever
 * vc_sever_due says so; with --track, every commit,
 * from incremental counts).  Two side branches give merge bases to find:
 * "old", one commit off the first commit (the base is the whole history
 * away), and "recent", one commit off ten commits back.
 *
 * At each milestone (1k, 10k, 100k, 1M, 10M commits) it prints, over the
 * interval since the last one: commit, save and sever cost; and, at the
 * milestone: log of 100, both merge bases, row diffs to commits 1, 1000
 * and half the history back, reopening the repo from its saved state,
 * the saved state's size, and memory.
 *
 * By default the window holds every commit (a depth beyond the history).
 * --keep-bytes, --keep-seconds (floors) and --max-commits, --max-bytes,
 * --max-seconds (ceilings) set a window instead; diffs then reach back at
 * most to the oldest commit held, and a merge base past the cut is -1.  --chunk sets the chunk size: the
 * table's own writes per commit do not depend on the window, so small
 * chunks keep a long run's disk down without changing what is measured.
 */
#define _DEFAULT_SOURCE
#include <getopt.h>
#include <inttypes.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../test/test_util.h"

static const char *ddl = "CREATE TABLE t (id int8 PRIMARY KEY, v int8)";

static double
maxrss_mb(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_maxrss / 1024.0;
}

static void
count_fn(void *arg, const ProllyDiff *d)
{
	(*(uint64_t *) arg)++;
}

static void
log_fn(void *arg, const VcCommit *c)
{
	(*(uint64_t *) arg)++;
}

/* Rows that differ between two commits' table, and the time to find them. */
static double
row_diff(VcRepo *r, const PHash *a, const PHash *b, uint64_t *rows)
{
	VcCommit	ca,
				cb;
	VcTable		ta,
				tb;
	double		t = now_sec();

	*rows = 0;
	CHECK(vc_commit_load(&r->ctx, a, &ca) && vc_commit_load(&r->ctx, b, &cb));
	CHECK(vc_table_get(&r->ctx, &ca.root, "t", &ta) && vc_table_get(&r->ctx, &cb.root, "t", &tb));
	CHECK_OK(prolly_diff(&r->ctx, &ta.data, &tb.data, count_fn, rows));
	return (now_sec() - t) * 1e3;
}

typedef struct Acc
{
	double		commit_s,
				save_s,
				sever_s;
	uint64_t	commits,
				saves,
				severs,
				save_bytes;
} Acc;

int
main(int argc, char **argv)
{
	uint64_t	rows = 100000,
				ncommits = 1000000;
	int			changes = 4,
				save_every = 1,
				flush_every = 1000,
				sever_every = 1000;
	bool		track = false,
				due = false;
	VcWindow	win = {0};
	ChunkPolicy policy = prolly_default_policy;
	const char *base = "/home/dustin/pgv2-bench/deepbench";
	static struct option opts[] = {
		{"rows", 1, 0, 'r'}, {"commits", 1, 0, 'c'}, {"changes", 1, 0, 'n'},
		{"save-every", 1, 0, 's'}, {"flush-every", 1, 0, 'f'},
		{"sever-every", 1, 0, 'v'}, {"track", 0, 0, 't'}, {"dir", 1, 0, 'd'},
		{"chunk", 1, 0, 'k'}, {"due", 0, 0, 'u'},
		{"keep-bytes", 1, 0, 'B'}, {"keep-seconds", 1, 0, 'S'},
		{"max-commits", 1, 0, 'C'}, {"max-bytes", 1, 0, 'X'}, {"max-seconds", 1, 0, 'T'},
		{0, 0, 0, 0}
	};
	int			ch;
	char		dir[600],
				err[256];
	ChunkStore *cs;
	VcRepo		r;
	PHash		rv,
				data,
				*heads,
				old_head;
	uint64_t	next_milestone = 1000;
	uint32_t	depth;
	bool		policy_mode;
	Acc			acc = {0};
	double		t0;

	while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1)
		switch (ch)
		{
			case 'r': rows = strtoull(optarg, NULL, 10); break;
			case 'c': ncommits = strtoull(optarg, NULL, 10); break;
			case 'n': changes = atoi(optarg); break;
			case 's': save_every = atoi(optarg); break;
			case 'f': flush_every = atoi(optarg); break;
			case 'v': sever_every = atoi(optarg); break;
			case 't': track = true; break;
			case 'u': due = true; break;
			case 'B': win.keep_bytes = strtoull(optarg, NULL, 10); break;
			case 'S': win.keep_seconds = strtoull(optarg, NULL, 10); break;
			case 'C': win.max_commits = strtoull(optarg, NULL, 10); break;
			case 'X': win.max_bytes = strtoull(optarg, NULL, 10); break;
			case 'T': win.max_seconds = strtoull(optarg, NULL, 10); break;
			case 'd': base = optarg; break;
			case 'k':
				policy.target_bytes = (uint32_t) atoi(optarg);
				policy.min_bytes = policy.target_bytes / 4;
				policy.max_bytes = policy.target_bytes * 4;
				break;
			default:
				fprintf(stderr, "deepbench [--rows N] [--commits N] [--changes N] [--save-every N]\n"
						"          [--flush-every N] [--sever-every N] [--due] [--track] [--dir D]\n"
						"          [--keep-bytes N] [--keep-seconds N] [--max-commits N]\n"
						"          [--max-bytes N] [--max-seconds N] [--chunk BYTES]\n");
				return 2;
		}
	depth = (uint32_t) (ncommits + 100);
	policy_mode = win.keep_bytes || win.keep_seconds || win.max_commits || win.max_bytes ||
		win.max_seconds;
	if (!policy_mode)
		win.depth = depth;
	mkdir(base, 0700);
	snprintf(dir, sizeof(dir), "%s/%ld-%d", base, (long) time(NULL), (int) getpid());
	mkdir(dir, 0700);
	cs = dirstore_open(dir, err, sizeof(err));
	CHECK(cs != NULL);
	rng_seed(7);
	vc_repo_init(&r, cs, &policy);
	rv = vc_working(&r);
	CHECK(vc_table_create(&r.ctx, &rv, "t", ddl, strlen(ddl), &rv) == 0);
	data = build_even_rows(&r.ctx, rows);
	CHECK(vc_table_set_data(&r.ctx, &rv, "t", &data, &rv) == 0);
	vc_set_working(&r, &rv);
	heads = px_malloc((ncommits + 1) * sizeof(PHash));
	heads[0] = vc_commit(&r, "load");

	/* "old": one commit off the first; its merge base with main is heads[0] */
	CHECK(vc_branch_create(&r, "old", &heads[0]) == VC_OK);
	CHECK(vc_checkout(&r, "old") == VC_OK);
	{
		EditBatch	b;
		PHash		w = vc_working(&r),
					o;

		batch_init(&b, 1);
		batch_put(&b, 1, 999999);
		CHECK(vc_table_edit(&r.ctx, &w, "t", b.e, b.n, &o) == 0);
		batch_free(&b);
		vc_set_working(&r, &o);
		old_head = vc_commit(&r, "old");
	}
	CHECK(vc_checkout(&r, "main") == VC_OK);
	if (track)
		CHECK(vc_window_track(&r, depth) == VC_OK);

	printf("# window: keep_bytes %" PRIu64 " keep_seconds %" PRIu64 " max_commits %" PRIu64
		   " max_bytes %" PRIu64 " max_seconds %" PRIu64 "%s\n", win.keep_bytes, win.keep_seconds,
		   win.max_commits, win.max_bytes, win.max_seconds, policy_mode ? "" : " (all held)");
	printf("# %" PRIu64 " rows, %u-byte chunks, %d changes per commit, save every %d, sever every %d%s, flush every %d\n",
		   rows, policy.target_bytes, changes, save_every, track ? 1 : sever_every, track ? " (tracked)" : "",
		   flush_every);
	printf("%9s %9s %7s %9s %9s %10s %9s %10s %10s %8s %9s %9s %10s %10s %10s %9s %9s %8s\n",
		   "commits", "held", "chg_B", "commit_us", "save_us", "save_KB", "severs", "sever_ms", "state_KB", "log100",
		   "mb_short", "mb_long", "diff1", "diff1k", "diffhalf", "open_ms", "rss_MB", "elapsed");
	t0 = now_sec();
	for (uint64_t i = 1; i <= ncommits; i++)
	{
		EditBatch	b;
		PHash		w = vc_working(&r),
					o;
		double		t = now_sec();

		batch_init(&b, (size_t) changes);
		for (int j = 0; j < changes; j++)
			batch_put(&b, rnd_below(rows) * 2, i);
		CHECK(vc_table_edit(&r.ctx, &w, "t", b.e, b.n, &o) == 0);
		batch_free(&b);
		vc_set_working(&r, &o);
		heads[i] = vc_commit(&r, "tx");
		acc.commit_s += now_sec() - t;
		acc.commits++;

		if (track || (due ? vc_sever_due(&r, &win) : i % (uint64_t) sever_every == 0))
		{
			uint32_t	seg;

			t = now_sec();
			CHECK(vc_sever(&r, &win, &seg) == VC_OK);
			if (seg != VC_NO_SEGMENT)
			{
				/* with no window set, only "recent"'s last commit is left behind */
				if (!policy_mode)
					CHECK_EQ(r.segs[seg].n, 1);
				CHECK(vc_segment_discard(&r, seg) == VC_OK);
			}
			acc.sever_s += now_sec() - t;
			acc.severs++;
		}
		if (i % (uint64_t) save_every == 0)
		{
			uint64_t	before = cs->stats.new_bytes;
			PHash		st;

			t = now_sec();
			st = vc_repo_save(&r);
			dirstore_set_state(cs, &st);
			acc.save_s += now_sec() - t;
			acc.save_bytes += cs->stats.new_bytes - before;
			acc.saves++;
		}
		if (i % (uint64_t) flush_every == 0)
			CHECK_OK(dirstore_flush(cs));

		if (i == next_milestone || i == ncommits)
		{
			uint64_t	n = 0,
						back,
						d1,
						d1k,
						dh;
			double		tl,
						ts,
						tlg,
						td1,
						td1k,
						tdh,
						topen;
			PHash		base,
						st;
			double		chg;
			size_t		stlen = 0;

			/* "recent": one commit off ten commits back */
			{
				EditBatch	e;
				PHash		w2,
							o2;

				CHECK(vc_branch_create(&r, "recent", &heads[i > 10 ? i - 10 : 0]) == VC_OK);
				CHECK(vc_checkout(&r, "recent") == VC_OK);
				w2 = vc_working(&r);
				batch_init(&e, 1);
				batch_put(&e, 3, i);
				CHECK(vc_table_edit(&r.ctx, &w2, "t", e.e, e.n, &o2) == 0);
				batch_free(&e);
				vc_set_working(&r, &o2);
				vc_commit(&r, "recent");
				CHECK(vc_checkout(&r, "main") == VC_OK);
			}

			tl = now_sec();
			{
				int			lrc = vc_log(&r, &heads[i], 100, 0, log_fn, &n);

				CHECK(lrc == VC_OK || lrc == VC_ERR_SEVERED);
			}
			tl = (now_sec() - tl) * 1e6;
			ts = now_sec();
			CHECK(vc_merge_base_in(&r, &heads[i], &vc_branch_find(&r, "recent")->head, 0, &base) == 1);
			CHECK(phash_eq(&base, &heads[i > 10 ? i - 10 : 0]));
			ts = (now_sec() - ts) * 1e6;
			tlg = now_sec();
			{
				int			mrc = vc_merge_base_in(&r, &heads[i], &old_head, 0, &base);

				tlg = (now_sec() - tlg) * 1e3;
				if (mrc == 1)
					CHECK(phash_eq(&base, &heads[0]));
				else
				{
					CHECK(policy_mode && (mrc == VC_ERR_SEVERED || mrc == 0));
					tlg = -1;
				}
			}
			/* as far back as main's own window reaches */
			back = i;
			if (policy_mode)
				for (back = 1; back < i &&
					 vc_locate(&r, &heads[i - back - 1], NULL) == VC_LOC_WINDOW; back++)
					;
			td1 = row_diff(&r, &heads[i - 1], &heads[i], &d1);
			td1k = row_diff(&r, &heads[i - (back < 1000 ? back : 1000)], &heads[i], &d1k);
			tdh = row_diff(&r, &heads[i - (back < i / 2 ? back : i / 2)], &heads[i], &dh);

			/* reopen from the saved state, as recovery would */
			st = vc_repo_save(&r);
			dirstore_set_state(cs, &st);
			CHECK(cs_get(cs, &st, &stlen) != NULL);
			{
				VcRepo		r2;

				topen = now_sec();
				CHECK(vc_repo_open(&r2, cs, &policy, &st) == VC_OK);
				topen = (now_sec() - topen) * 1e3;
				CHECK(phash_eq(&vc_head(&r2)->head, &heads[i]));
				vc_repo_free(&r2);
			}
			/* "recent" goes, so its window does not hold main's history */
			CHECK(vc_branch_delete(&r, "recent") == VC_OK);
			{
				VcCommit	hc,
							pc;

				CHECK(vc_commit_load(&r.ctx, &heads[i], &hc) &&
					  vc_commit_load(&r.ctx, &heads[i - 1], &pc));
				chg = (double) (hc.bytes - pc.bytes);
			}
			printf("%9" PRIu64 " %9" PRIu64 " %7.0f %9.1f %9.1f %10.1f %9" PRIu64 " %10.1f %10.1f %8.0f %9.0f %9.1f "
				   "%5.1f/%-4" PRIu64 " %5.1f/%-4" PRIu64 " %5.1f/%-5" PRIu64 " %9.1f %9.0f %8.0f\n",
				   i, r.nheld, chg, (acc.commit_s + acc.sever_s) / acc.commits * 1e6,
				   acc.saves ? acc.save_s / acc.saves * 1e6 : 0,
				   acc.saves ? acc.save_bytes / (double) acc.saves / 1024.0 : 0,
				   (uint64_t) acc.severs, acc.severs ? acc.sever_s / acc.severs * 1e3 : 0,
				   stlen / 1024.0, tl, ts, tlg, td1, d1, td1k, d1k, tdh, dh, topen,
				   maxrss_mb(), now_sec() - t0);
			fflush(stdout);
			memset(&acc, 0, sizeof(acc));
			next_milestone *= 10;
		}
	}
	printf("# commit_us includes severing; severs in the interval, sever_ms per sever; log100,\n"
		   "# mb_short in us; mb_long in ms (-1: past the cut); diffs ms/rows; disk %.0f MB\n",
		   dirstore_disk_bytes(cs) / 1048576.0);
	free(heads);
	vc_repo_free(&r);
	cs_destroy(cs);
	return 0;
}
