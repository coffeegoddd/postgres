/*
 * test_kfplan.c
 *	  The keyframe planner: checkouts take the cheaper of replaying rows
 *	  and restoring a keyframe, keyframes are captured only when restoring
 *	  the change since the last would beat replaying it, warm pages make
 *	  replay cheaper, and a calibrating planner learns a server's costs.
 *
 * Accuracy is shown two ways: on a simulated server whose costs the
 * planner does not know (and which are not quite linear), by the time it
 * loses against always choosing best; and on the moves bench/kfbench timed
 * on a real server, by predicting each one from all the others.
 */
#include "test_util.h"
#include "../kfplan.h"

/*
 * Costs shaped like the server's (bench/kfbench, wal_compression off):
 * replay pays per row and per page it dirties, much less for a page
 * already written since the last checkpoint; restore pays per page and a
 * fixed cost for its lock and truncation.
 */
static const KfCosts costs = {
	.replay_fixed_us = 0, .row_us = 7, .replay_page_us = 22, .replay_warm_page_us = 9,
	.restore_fixed_us = 6000, .page_us = 14, .capture_page_us = 4
};

static KfMove
mv(uint64_t rows, uint64_t pages, uint64_t warm)
{
	KfMove		m = {rows, pages, warm};

	return m;
}

static KfCandidate
cand(uint64_t pages, KfMove after)
{
	KfCandidate c;

	memset(&c, 0, sizeof(c));
	c.keyframe.b[0] = (uint8_t) pages;
	c.pages = pages;
	c.after = after;
	return c;
}

static double
absd(double x)
{
	return x < 0 ? -x : x;
}

/* ------------------------------------------------------- fixed costs */

/* Small changes replay; large ones restore, sparse or dense. */
static void
test_size_and_spread(void)
{
	KfPlanner  *p = kf_cost_planner(&costs);
	KfMove		m;
	KfCandidate c;
	KfChoice	ch;

	/* 100 rows on 300 pages: the restore's fixed cost dominates, replay */
	m = mv(100, 300, 0);
	c = cand(300, mv(0, 0, 0));
	ch = p->ops->choose(p, &m, &c, 1);
	CHECK(ch.path == KF_REPLAY && !ch.explored);
	CHECK(ch.est_us == ch.replay_us);
	CHECK_EQ((uint64_t) ch.est_us, (uint64_t) (7 * 100 + 22 * 300));
	/* 10,000 rows on 13,000 cold pages: restore, though only a row a page */
	m = mv(10000, 13000, 0);
	c = cand(13000, mv(0, 0, 0));
	ch = p->ops->choose(p, &m, &c, 1);
	CHECK(ch.path == KF_RESTORE && ch.candidate == 0);
	CHECK_EQ((uint64_t) ch.est_us, (uint64_t) (6000 + 14 * 13000));
	/* 100,000 rows on 5,000 pages: restore, by far */
	m = mv(100000, 5000, 0);
	c = cand(5000, mv(0, 0, 0));
	ch = p->ops->choose(p, &m, &c, 1);
	CHECK(ch.path == KF_RESTORE);
	CHECK(ch.replay_us > 8 * ch.est_us);
	/* nothing to do costs nothing, and needs no keyframe */
	m = mv(0, 0, 0);
	ch = p->ops->choose(p, &m, &c, 1);
	CHECK(ch.path == KF_REPLAY && ch.est_us == 0);
	/* no candidates: replay */
	m = mv(100000, 5000, 0);
	ch = p->ops->choose(p, &m, NULL, 0);
	CHECK(ch.path == KF_REPLAY);
	p->ops->destroy(p);
}

/* Pages written since the last checkpoint make replay cheaper; restore is unchanged. */
static void
test_warm_pages(void)
{
	KfPlanner  *p = kf_cost_planner(&costs);
	KfMove		cold = mv(10000, 13000, 0),
				warm = mv(10000, 13000, 13000),
				half = mv(10000, 13000, 6500);
	KfCandidate c = cand(13000, mv(0, 0, 0));

	CHECK(p->ops->choose(p, &cold, &c, 1).path == KF_RESTORE);
	CHECK(p->ops->choose(p, &warm, &c, 1).path == KF_REPLAY);
	CHECK_EQ((uint64_t) kf_replay_us(&costs, &half),
			 (uint64_t) (7 * 10000 + 22 * 6500 + 9 * 6500));
	/* warm can't exceed the pages: more is read as all of them */
	{
		KfMove		over = mv(10000, 13000, 99999);

		CHECK(kf_replay_us(&costs, &over) == kf_replay_us(&costs, &warm));
	}
	/* a dense change still restores, warm or not */
	{
		KfMove		dense = mv(100000, 5000, 5000);
		KfCandidate d = cand(5000, mv(0, 0, 0));

		CHECK(p->ops->choose(p, &dense, &d, 1).path == KF_RESTORE);
	}
	p->ops->destroy(p);
}

/* A restore pays for the rows replayed after it; the cheapest candidate wins. */
static void
test_best_candidate(void)
{
	KfPlanner  *p = kf_cost_planner(&costs);
	KfCandidate c[3];
	KfMove		m = mv(200000, 20000, 0);
	KfChoice	ch;

	c[0] = cand(2000, mv(50000, 8000, 8000));	/* near in pages, far in rows */
	c[1] = cand(3000, mv(100, 100, 100));	/* a little further, nearly there */
	c[2] = cand(100000, mv(0, 0, 0));	/* the target itself, but every page differs */
	ch = p->ops->choose(p, &m, c, 3);
	CHECK(ch.path == KF_RESTORE && ch.candidate == 1);
	/* when every candidate costs more than replay, replay */
	m = mv(100, 100, 0);
	ch = p->ops->choose(p, &m, c, 3);
	CHECK(ch.path == KF_REPLAY);
	/* a tie goes to replay */
	{
		KfCandidate t = cand(0, mv(0, 0, 0));
		KfCosts		even = costs;

		even.restore_fixed_us = 7 * 100 + 22 * 100;
		p->ops->destroy(p);
		p = kf_cost_planner(&even);
		ch = p->ops->choose(p, &m, &t, 1);
		CHECK(ch.path == KF_REPLAY);
	}
	p->ops->destroy(p);
}

/* Capture once restoring the change since the last keyframe beats replaying it. */
static void
test_capture(void)
{
	KfPlanner  *p = kf_cost_planner(&costs);
	KfMove		m;

	m = mv(50, 100, 0);			/* small: never */
	CHECK(!p->ops->should_capture(p, &m));
	m = mv(50000, 1000, 0);		/* large: yes */
	CHECK(p->ops->should_capture(p, &m));
	m = mv(10000, 13000, 0);
	CHECK(p->ops->should_capture(p, &m));
	m = mv(0, 0, 0);			/* nothing changed: nothing to capture */
	CHECK(!p->ops->should_capture(p, &m));
	/* capturing costs its pages: just past break-even it does not pay yet */
	{
		uint64_t	pages = 100;
		double		restore = 6000 + 14.0 * (double) pages;
		uint64_t	rows = (uint64_t) ((restore - 22.0 * (double) pages) / 7) + 10;

		m = mv(rows, pages, 0);
		CHECK(kf_replay_us(&costs, &m) > restore);
		CHECK(!p->ops->should_capture(p, &m));
		m.rows += (4 * pages) / 7 + 10;
		CHECK(p->ops->should_capture(p, &m));
	}
	/* monotone: more rows on the same pages only makes it more worth it */
	{
		bool		was = false;

		for (uint64_t rows = 0; rows <= 100000; rows += 50)
		{
			bool		now;

			m = mv(rows, 200, 0);
			now = p->ops->should_capture(p, &m);
			CHECK(!was || now);
			was = now;
		}
		CHECK(was);
	}
	p->ops->destroy(p);
}

/* The break-even row count, and none when restoring a page costs too much. */
static void
test_break_even(void)
{
	double		r = kf_break_even_rows(&costs, 0.5);	/* two rows a page */
	KfPlanner  *p = kf_cost_planner(&costs);
	KfMove		m;
	KfCandidate c;

	CHECK(r > 0);
	/* just past it restore wins, just short of it replay does */
	m = mv((uint64_t) (r * 1.1), (uint64_t) (r * 1.1 * 0.5), 0);
	c = cand(m.pages, mv(0, 0, 0));
	CHECK(p->ops->choose(p, &m, &c, 1).path == KF_RESTORE);
	m = mv((uint64_t) (r * 0.9), (uint64_t) (r * 0.9 * 0.5), 0);
	c = cand(m.pages, mv(0, 0, 0));
	CHECK(p->ops->choose(p, &m, &c, 1).path == KF_REPLAY);
	p->ops->destroy(p);
	/* a page restore costing 150 us against 8 per row and nothing per page */
	{
		KfCosts		dear = {.row_us = 8, .page_us = 150, .restore_fixed_us = 1000};

		CHECK(kf_break_even_rows(&dear, 5) < 0);
		CHECK(kf_break_even_rows(&dear, 0.02) > 0);
	}
}

/* ------------------------------------------------------- calibration */

/*
 * A server the planner does not know: linear in the large, but small
 * replays cost less per page than a line says (as measured), with noise.
 */
typedef struct Server
{
	KfCosts		c;
} Server;

static double
noise(void)
{
	return 0.95 + 0.1 * (double) rnd_below(1000) / 1000.0;
}

static double
true_replay(const Server *s, const KfMove *m)
{
	double		us = kf_replay_us(&s->c, m);
	double		small = m->pages < 3000 ? 0.6 + 0.4 * (double) m->pages / 3000.0 : 1.0;

	return us * small;
}

static double
true_restore(const Server *s, uint64_t pages)
{
	return s->c.restore_fixed_us + s->c.page_us * (double) pages;
}

/* A random move: rows from 10 to a million, spread from dense to sparse. */
static KfMove
random_move(uint64_t table_pages)
{
	uint64_t	rows = 10;
	double		ppr = 0.02 * (double) (1 + rnd_below(150));	/* 0.02 .. 3 pages a row */
	uint64_t	pages;
	KfMove		m;

	for (int e = (int) rnd_below(6); e > 0; e--)
		rows *= 10;
	rows = rows + rnd_below(rows * 9);
	pages = (uint64_t) ((double) rows * ppr) + 1;
	if (pages > table_pages)
		pages = table_pages;
	m = mv(rows, pages, rnd_below(3) == 0 ? 0 : rnd_below(pages + 1));
	return m;
}

typedef struct Run
{
	double		regret,			/* time lost against always choosing best */
				best;			/* time always choosing best would take */
	int			right,			/* on decisions that matter */
				n,
				late_right,
				late_n,
				explored;
	double		late_regret,
				late_best;
} Run;

/*
 * Runs moves against a server: the planner chooses, the server takes the
 * true time of what it chose, and the planner observes it.  The last
 * `late` moves are scored apart, after it has had time to learn.
 */
static void
run_moves(KfPlanner *p, const Server *s, int moves, int late, Run *out)
{
	memset(out, 0, sizeof(*out));
	for (int i = 0; i < moves; i++)
	{
		KfMove		m = random_move(25000);
		KfCandidate c = cand(m.pages, mv(0, 0, 0));
		KfChoice	ch = p->ops->choose(p, &m, &c, 1);
		double		r = true_replay(s, &m) * noise(),
					t = true_restore(s, m.pages) * noise(),
					got = ch.path == KF_REPLAY ? r : t,
					best = r < t ? r : t;

		if (ch.path == KF_REPLAY)
			p->ops->observe_replay(p, &m, got);
		else
			p->ops->observe_restore(p, m.pages, got);
		/* a decision that matters: one path 20% cheaper, and over a millisecond at stake */
		bool		clear = (r > t ? r : t) > 1.2 * best && (r > t ? r - t : t - r) > 1000;

		out->regret += got - best;
		out->best += best;
		out->right += clear && got == best;
		out->n += clear;
		out->explored += ch.explored;
		if (i >= moves - late)
		{
			out->late_regret += got - best;
			out->late_best += best;
			out->late_right += clear && got == best;
			out->late_n += clear;
		}
	}
}

/* pglz-like costs: what a planner measured on another server would assume. */
static const KfCosts wrong_prior = {
	.replay_fixed_us = 65000, .row_us = 8, .replay_page_us = 74, .replay_warm_page_us = 11,
	.restore_fixed_us = 120000, .page_us = 54, .capture_page_us = 5
};

static const KfCalibration cal = {
	.forget = 0.99, .prior_weight = 1, .explore_every = 50
};

/*
 * Started from another server's costs, the calibrating planner loses far
 * less time than a planner that keeps them, and once it has learned it
 * picks the faster path almost always and loses almost nothing.
 */
static void
test_calibration_learns(void)
{
	Server		s = {costs};
	KfPlanner  *fixed = kf_cost_planner(&wrong_prior),
			   *learn = kf_calibrating_planner(&wrong_prior, &cal);
	Run			rf,
				rl;
	KfCosts		got;

	rng_seed(101);
	run_moves(fixed, &s, 1500, 500, &rf);
	rng_seed(101);
	run_moves(learn, &s, 1500, 500, &rl);
	printf("\n    clear decisions right, and time lost against always choosing best:"
		   "\n    fixed: %d/%d, %.1f%%; calibrating: %d/%d, %.1f%%, late %d/%d, %.2f%%, explored %d\n%-44s",
		   rf.right, rf.n, 100 * rf.regret / rf.best, rl.right, rl.n,
		   100 * rl.regret / rl.best, rl.late_right, rl.late_n,
		   100 * rl.late_regret / rl.late_best, rl.explored, "");
	/* learning from a prior that wrong costs something, but far less than keeping it */
	CHECK(rl.regret < 0.5 * rf.regret);
	CHECK(rl.late_right >= rl.late_n * 95 / 100);
	CHECK(rl.late_regret < 0.02 * rl.late_best);
	/* exploring is rare, and only where it costs little */
	CHECK(rl.explored < rl.n / 10);
	/*
	 * The server is not linear, so what it learns is the best line, not the
	 * server's constants; the cost that decides large moves, a restored
	 * page's, comes out close.
	 */
	learn->ops->costs(learn, &got);
	CHECK(absd(got.page_us - costs.page_us) < 0.25 * costs.page_us);
	fixed->ops->destroy(fixed);
	learn->ops->destroy(learn);
}

/* When the server changes under it (compression switched on), it follows. */
static void
test_calibration_follows_drift(void)
{
	Server		off = {costs},
				pglz = {wrong_prior};
	KfPlanner  *learn = kf_calibrating_planner(&costs, &cal);
	Run			before,
				after;

	rng_seed(202);
	run_moves(learn, &off, 600, 300, &before);
	run_moves(learn, &pglz, 1200, 400, &after);
	printf("\n    before: late right %d/%d; after the switch: late right %d/%d, lost %.2f%%\n%-44s",
		   before.late_right, before.late_n, after.late_right, after.late_n,
		   100 * after.late_regret / after.late_best, "");
	CHECK(before.late_right >= before.late_n * 95 / 100);
	CHECK(after.late_right >= after.late_n * 95 / 100);
	CHECK(after.late_regret < 0.03 * after.late_best);
	learn->ops->destroy(learn);
}

/*
 * Without exploration it never learns the path it never takes: from a
 * prior that prices replay far too high, it would restore every small move
 * at several milliseconds each, when replaying would take a fraction of
 * one.  Little of the total time, but most of the decisions.
 */
static void
test_exploration_matters(void)
{
	Server		s = {costs};
	KfCalibration none = cal;
	KfPlanner  *blind,
			   *curious;
	Run			rb,
				rc;

	none.explore_every = 0;
	blind = kf_calibrating_planner(&wrong_prior, &none);
	curious = kf_calibrating_planner(&wrong_prior, &cal);
	rng_seed(303);
	run_moves(blind, &s, 1500, 500, &rb);
	rng_seed(303);
	run_moves(curious, &s, 1500, 500, &rc);
	printf("\n    no exploring: late right %d/%d; exploring: late right %d/%d\n%-44s",
		   rb.late_right, rb.late_n, rc.late_right, rc.late_n, "");
	CHECK(rb.late_right < rb.late_n * 9 / 10);
	CHECK(rc.late_right >= rc.late_n * 95 / 100);
	blind->ops->destroy(blind);
	curious->ops->destroy(curious);
}

/* ---------------------------------------------------- measured moves */

/*
 * Moves bench/kfbench timed on a real server (1M rows, two indexes): each
 * change shape moved back by replay and by restore, after a checkpoint
 * (cold) and without one (warm, every page already written since).
 */
typedef struct Measured
{
	uint64_t	rows,
				pages;
	bool		warm;
	double		replay_ms,
				restore_ms;
} Measured;

static const Measured off_moves[] = {
	{100, 296, false, 5.7, 11.2}, {1000, 2641, false, 33.6, 41.2},
	{10000, 13018, false, 239.4, 189.9}, {100000, 23618, false, 1262.4, 323.8},
	{1000, 1027, false, 15.1, 21.5}, {10000, 2947, false, 100.8, 45.7},
	{100000, 4832, false, 858.3, 76.7}, {1000000, 23671, false, 7684.8, 355.1},
	{100, 296, true, 2.2, 10.7}, {1000, 2641, true, 12.0, 42.5},
	{10000, 13018, true, 98.6, 203.8}, {100000, 23618, true, 1000.1, 349.7},
	{1000, 1027, true, 8.8, 23.1}, {10000, 2947, true, 65.1, 47.6},
	{100000, 4832, true, 784.8, 78.1}, {1000000, 23671, true, 7451.4, 362.7},
};

static const Measured pglz_moves[] = {
	{100, 296, false, 27.6, 31.8}, {1000, 2641, false, 241.6, 239.9},
	{10000, 13018, false, 1060.8, 937.3}, {100000, 23618, false, 2638.3, 1339.7},
	{1000, 1027, false, 128.2, 134.0}, {10000, 2947, false, 419.8, 347.3},
	{100000, 4832, false, 1323.9, 443.3}, {1000000, 23671, false, 9869.9, 1393.6},
	{100, 296, true, 3.2, 34.1}, {1000, 2641, true, 12.5, 243.9},
	{10000, 13018, true, 102.4, 940.2}, {100000, 23618, true, 1015.9, 1374.8},
	{1000, 1027, true, 9.9, 132.8}, {10000, 2947, true, 75.5, 357.5},
	{100000, 4832, true, 799.5, 448.1}, {1000000, 23671, true, 7252.4, 1388.8},
};

static const Measured lz4_moves[] = {
	{100, 296, false, 5.1, 8.9}, {1000, 2641, false, 35.8, 45.8},
	{10000, 13018, false, 192.3, 145.7}, {100000, 23618, false, 1290.6, 244.5},
	{1000, 1027, false, 17.0, 25.6}, {10000, 2947, false, 112.0, 52.0},
	{100000, 4832, false, 963.9, 72.6}, {1000000, 23671, false, 8057.1, 253.8},
	{100, 296, true, 2.2, 9.0}, {1000, 2641, true, 11.9, 44.7},
	{10000, 13018, true, 98.4, 144.4}, {100000, 23618, true, 1040.6, 232.8},
	{1000, 1027, true, 8.9, 20.9}, {10000, 2947, true, 72.4, 50.3},
	{100000, 4832, true, 813.7, 74.7}, {1000000, 23671, true, 7173.3, 250.4},
};

/*
 * Each move predicted from all the others: a calibrating planner, started
 * from deliberately poor costs, observes every other move both ways, then
 * chooses for the one left out.  It must pick the faster path almost
 * always, and where it does not, lose little.
 */
static void
leave_one_out(const char *name, const Measured *ms, int n, int *right, double *lost_pct)
{
	static const KfCosts poor = {
		.replay_fixed_us = 1000, .row_us = 8, .replay_page_us = 0, .replay_warm_page_us = 0,
		.restore_fixed_us = 1000, .page_us = 150, .capture_page_us = 5
	};
	KfCalibration c = {.forget = 1, .prior_weight = 0.5, .explore_every = 0};
	double		lost = 0,
				best = 0;

	*right = 0;
	for (int out = 0; out < n; out++)
	{
		KfPlanner  *p = kf_calibrating_planner(&poor, &c);
		const Measured *t = &ms[out];
		KfMove		m = mv(t->rows, t->pages, t->warm ? t->pages : 0);
		KfCandidate k = cand(t->pages, mv(0, 0, 0));
		KfChoice	ch;
		double		got,
					b;

		for (int i = 0; i < n; i++)
			if (i != out)
			{
				KfMove		mi = mv(ms[i].rows, ms[i].pages, ms[i].warm ? ms[i].pages : 0);

				p->ops->observe_replay(p, &mi, ms[i].replay_ms * 1000);
				p->ops->observe_restore(p, ms[i].pages, ms[i].restore_ms * 1000);
			}
		ch = p->ops->choose(p, &m, &k, 1);
		got = ch.path == KF_REPLAY ? t->replay_ms : t->restore_ms;
		b = t->replay_ms < t->restore_ms ? t->replay_ms : t->restore_ms;
		*right += got == b;
		lost += got - b;
		best += b;
		if (got != b)
			printf("\n    %s: missed %llu rows on %llu %s pages: %.1f ms for %.1f",
				   name, (unsigned long long) t->rows, (unsigned long long) t->pages,
				   t->warm ? "warm" : "cold", got, b);
		p->ops->destroy(p);
	}
	*lost_pct = 100 * lost / best;
}

static void
test_measured_moves(void)
{
	static const struct
	{
		const char *name;
		const Measured *m;
		int			n;
	}			sets[] = {
		{"off", off_moves, (int) (sizeof(off_moves) / sizeof(off_moves[0]))},
		{"pglz", pglz_moves, (int) (sizeof(pglz_moves) / sizeof(pglz_moves[0]))},
		{"lz4", lz4_moves, (int) (sizeof(lz4_moves) / sizeof(lz4_moves[0]))},
	};

	for (int i = 0; i < 3; i++)
	{
		int			right;
		double		lost;

		leave_one_out(sets[i].name, sets[i].m, sets[i].n, &right, &lost);
		printf("\n    %s, each left out and predicted: %d/%d right, %.2f%% lost",
			   sets[i].name, right, sets[i].n, lost);
		CHECK(right >= sets[i].n - 2);
		CHECK(lost < 2);
	}
	printf("\n%-44s", "");
}

int
main(void)
{
	RUN(test_size_and_spread);
	RUN(test_warm_pages);
	RUN(test_best_candidate);
	RUN(test_capture);
	RUN(test_break_even);
	RUN(test_calibration_learns);
	RUN(test_calibration_follows_drift);
	RUN(test_exploration_matters);
	RUN(test_measured_moves);
	printf("test_kfplan: all tests passed\n");
	return 0;
}
