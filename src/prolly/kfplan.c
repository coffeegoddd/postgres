/*
 * kfplan.c
 *	  Keyframe planners: one that prices each path with fixed costs, and
 *	  one that refits those costs from what moves actually take.
 */
#include "kfplan.h"

static double
absd(double x)
{
	return x < 0 ? -x : x;
}

/* Newton's square root, so the library needs no libm. */
static double
sqrt_(double x)
{
	double		r = x > 1 ? x : 1;

	if (x <= 0)
		return 0;
	for (int i = 0; i < 60; i++)
		r = 0.5 * (r + x / r);
	return r;
}

double
kf_replay_us(const KfCosts *c, const KfMove *m)
{
	uint64_t	warm = m->warm_pages < m->pages ? m->warm_pages : m->pages;

	if (m->rows == 0)
		return 0;
	return c->replay_fixed_us + c->row_us * (double) m->rows +
		c->replay_page_us * (double) (m->pages - warm) +
		c->replay_warm_page_us * (double) warm;
}

double
kf_restore_us(const KfCosts *c, const KfCandidate *k)
{
	return c->restore_fixed_us + c->page_us * (double) k->pages + kf_replay_us(c, &k->after);
}

double
kf_break_even_rows(const KfCosts *c, double pages_per_row)
{
	double		per_row = c->row_us + (c->replay_page_us - c->page_us) * pages_per_row;
	double		fixed = c->restore_fixed_us - c->replay_fixed_us;

	if (per_row <= 0)
		return -1;				/* restore never wins */
	return fixed > 0 ? fixed / per_row : 0;
}

/* The cheapest path under costs c; ties go to replay. */
static KfChoice
cheapest(const KfCosts *c, const KfMove *m, const KfCandidate *k, size_t n)
{
	double		r = kf_replay_us(c, m);
	KfChoice	ch = {KF_REPLAY, 0, r, r, false};

	for (size_t i = 0; i < n; i++)
	{
		double		est = kf_restore_us(c, &k[i]);

		if (est < ch.est_us)
		{
			ch.path = KF_RESTORE;
			ch.candidate = i;
			ch.est_us = est;
		}
	}
	return ch;
}

/*
 * A keyframe here pays when restoring the change since the last one costs
 * less than replaying it, by more than capturing costs.
 */
static bool
worth_capturing(const KfCosts *c, const KfMove *since)
{
	double		restore = c->restore_fixed_us + c->page_us * (double) since->pages;
	double		capture = c->capture_page_us * (double) since->pages;

	return since->rows > 0 && restore + capture < kf_replay_us(c, since);
}

/* ------------------------------------------------------------ fixed costs */

typedef struct CostPlanner
{
	KfPlanner	p;
	KfCosts		c;
} CostPlanner;

static KfChoice
cost_choose(KfPlanner *p, const KfMove *m, const KfCandidate *c, size_t n)
{
	return cheapest(&((CostPlanner *) p)->c, m, c, n);
}

static bool
cost_should_capture(KfPlanner *p, const KfMove *since)
{
	return worth_capturing(&((CostPlanner *) p)->c, since);
}

static void
cost_observe_replay(KfPlanner *p, const KfMove *m, double us)
{
}

static void
cost_observe_restore(KfPlanner *p, uint64_t pages, double us)
{
}

static void
cost_costs(KfPlanner *p, KfCosts *out)
{
	*out = ((CostPlanner *) p)->c;
}

static void
cost_destroy(KfPlanner *p)
{
	free(p);
}

static const KfPlannerOps cost_ops = {
	cost_choose, cost_should_capture, cost_observe_replay, cost_observe_restore,
	cost_costs, cost_destroy
};

KfPlanner *
kf_cost_planner(const KfCosts *costs)
{
	CostPlanner *p = px_malloc(sizeof(CostPlanner));

	p->p.ops = &cost_ops;
	p->c = *costs;
	return &p->p;
}

/* ------------------------------------------------------------ calibrating */

#define FIT_MAX 4

/*
 * One path's linear model y = theta . x, fitted by decaying least squares
 * with each observation weighted by 1 / y.  Plain least squares fits the
 * large moves and lets the fixed cost drift; relative error (1 / y^2)
 * lets the many small moves, where the line is only an approximation (a
 * replay's small moves cost less per page than its large ones), pull the
 * costs that decide the large ones.  1 / y sits between.
 * A and b hold the weighted sums of x x' and x y, n the decayed count of
 * observations.  The prior enters as a ridge toward theta0, scaled by each
 * feature's typical weighted size (|x| / y), so that it weighs like
 * prior_weight observations whatever the units.
 */
typedef struct Fit
{
	int			k;
	double		A[FIT_MAX][FIT_MAX];
	double		b[FIT_MAX];
	double		n;
	double		scale[FIT_MAX];	/* running mean of each |x| sqrt(w) */
	double		theta0[FIT_MAX];
	double		theta[FIT_MAX];
} Fit;

typedef struct CalPlanner
{
	KfPlanner	p;
	KfCalibration cal;
	KfCosts		c;				/* current */
	Fit			replay;			/* 1, rows, cold pages, warm pages */
	Fit			restore;		/* 1, pages */
	int			since_replay;	/* decisions since each path was taken */
	int			since_restore;
	double		avg_best_us;	/* running average of the cheapest estimate */
} CalPlanner;

static void
fit_init(Fit *f, int k, const double *theta0, const double *scale0)
{
	memset(f, 0, sizeof(*f));
	f->k = k;
	for (int j = 0; j < k; j++)
	{
		f->theta0[j] = f->theta[j] = theta0[j];
		f->scale[j] = scale0[j];
	}
}

/* Solves (A + ridge) theta = b + ridge theta0, with every theta >= 0. */
static void
fit_solve(Fit *f, double prior_weight)
{
	bool		free_[FIT_MAX];
	int			k = f->k;

	for (int j = 0; j < k; j++)
		free_[j] = true;
	for (int round = 0; round <= k; round++)
	{
		double		M[FIT_MAX][FIT_MAX + 1];
		int			idx[FIT_MAX],
					m = 0;
		bool		again = false;

		for (int j = 0; j < k; j++)
			if (free_[j])
				idx[m++] = j;
		for (int a = 0; a < m; a++)
		{
			int			ja = idx[a];
			double		ridge = prior_weight * f->scale[ja] * f->scale[ja];

			/* coefficients held at zero drop out */
			for (int b = 0; b < m; b++)
				M[a][b] = f->A[ja][idx[b]] + (a == b ? ridge : 0);
			M[a][m] = f->b[ja] + ridge * f->theta0[ja];
		}
		for (int a = 0; a < m; a++)
		{
			int			piv = a;

			for (int r = a + 1; r < m; r++)
				if (absd(M[r][a]) > absd(M[piv][a]))
					piv = r;
			if (piv != a)
				for (int col = 0; col <= m; col++)
				{
					double		t = M[a][col];

					M[a][col] = M[piv][col];
					M[piv][col] = t;
				}
			if (M[a][a] == 0)
				continue;
			for (int r = 0; r < m; r++)
				if (r != a)
				{
					double		q = M[r][a] / M[a][a];

					for (int col = a; col <= m; col++)
						M[r][col] -= q * M[a][col];
				}
		}
		for (int j = 0; j < k; j++)
			if (!free_[j])
				f->theta[j] = 0;
		for (int a = 0; a < m; a++)
		{
			double		t = M[a][a] != 0 ? M[a][m] / M[a][a] : f->theta0[idx[a]];

			f->theta[idx[a]] = t;
			if (t < 0)
			{
				free_[idx[a]] = false;
				again = true;
			}
		}
		if (!again)
			return;
	}
	for (int j = 0; j < k; j++)
		if (f->theta[j] < 0)
			f->theta[j] = 0;
}

static void
fit_observe(Fit *f, const double *x, double y, double forget, double prior_weight)
{
	double		yy = y > 1 ? y : 1,
				w = 1 / yy;
	double		keep = f->n > 0 ? forget : 0;

	f->n = f->n * forget + 1;
	for (int i = 0; i < f->k; i++)
	{
		/* the scale follows the data, so the prior keeps its weight */
		f->scale[i] = f->scale[i] * keep + (1 - keep) * absd(x[i]) * sqrt_(w);
		f->b[i] = f->b[i] * forget + w * x[i] * y;
		for (int j = 0; j < f->k; j++)
			f->A[i][j] = f->A[i][j] * forget + w * x[i] * x[j];
	}
	fit_solve(f, prior_weight);
}

static void
cal_refresh(CalPlanner *p)
{
	p->c.replay_fixed_us = p->replay.theta[0];
	p->c.row_us = p->replay.theta[1];
	p->c.replay_page_us = p->replay.theta[2];
	p->c.replay_warm_page_us = p->replay.theta[3];
	p->c.restore_fixed_us = p->restore.theta[0];
	p->c.page_us = p->restore.theta[1];
}

static KfChoice
cal_choose(KfPlanner *pl, const KfMove *m, const KfCandidate *c, size_t n)
{
	CalPlanner *p = (CalPlanner *) pl;
	KfChoice	ch = cheapest(&p->c, m, c, n);

	/*
	 * A path not taken for explore_every decisions is taken on the next
	 * small move: cheap to try, and what says most about a fixed cost.
	 */
	if (p->cal.explore_every > 0 && n > 0 && m->rows > 0)
	{
		bool		replay_best = ch.path == KF_REPLAY;
		bool		starved = (replay_best ? p->since_restore : p->since_replay) >=
			p->cal.explore_every && ch.est_us <= 0.25 * p->avg_best_us;

		p->avg_best_us = p->avg_best_us == 0 ? ch.est_us : 0.95 * p->avg_best_us + 0.05 * ch.est_us;
		if (starved)
		{
			size_t		oc = 0;
			double		other = ch.replay_us;

			if (replay_best)
			{
				other = kf_restore_us(&p->c, &c[0]);
				for (size_t i = 1; i < n; i++)
					if (kf_restore_us(&p->c, &c[i]) < other)
					{
						other = kf_restore_us(&p->c, &c[i]);
						oc = i;
					}
			}
			ch.path = replay_best ? KF_RESTORE : KF_REPLAY;
			ch.candidate = oc;
			ch.est_us = other;
			ch.explored = true;
		}
	}
	if (ch.path == KF_REPLAY)
	{
		p->since_replay = 0;
		p->since_restore++;
	}
	else
	{
		p->since_restore = 0;
		p->since_replay++;
	}
	return ch;
}

static bool
cal_should_capture(KfPlanner *pl, const KfMove *since)
{
	return worth_capturing(&((CalPlanner *) pl)->c, since);
}

static void
cal_observe_replay(KfPlanner *pl, const KfMove *m, double us)
{
	CalPlanner *p = (CalPlanner *) pl;
	uint64_t	warm = m->warm_pages < m->pages ? m->warm_pages : m->pages;
	double		x[4] = {1, (double) m->rows, (double) (m->pages - warm), (double) warm};

	if (m->rows == 0)
		return;
	fit_observe(&p->replay, x, us, p->cal.forget, p->cal.prior_weight);
	cal_refresh(p);
}

static void
cal_observe_restore(KfPlanner *pl, uint64_t pages, double us)
{
	CalPlanner *p = (CalPlanner *) pl;
	double		x[2] = {1, (double) pages};

	fit_observe(&p->restore, x, us, p->cal.forget, p->cal.prior_weight);
	cal_refresh(p);
}

static void
cal_costs(KfPlanner *pl, KfCosts *out)
{
	*out = ((CalPlanner *) pl)->c;
}

static void
cal_destroy(KfPlanner *pl)
{
	free(pl);
}

static const KfPlannerOps cal_ops = {
	cal_choose, cal_should_capture, cal_observe_replay, cal_observe_restore,
	cal_costs, cal_destroy
};

KfPlanner *
kf_calibrating_planner(const KfCosts *prior, const KfCalibration *cal)
{
	CalPlanner *p = px_malloc(sizeof(CalPlanner));
	double		rt[4] = {prior->replay_fixed_us, prior->row_us, prior->replay_page_us,
	prior->replay_warm_page_us};
	double		st[2] = {prior->restore_fixed_us, prior->page_us};

	/* the scales start from a move of a thousand rows on a thousand pages */
	KfMove		typical = {1000, 1000, 500};
	KfCandidate tk = {{{0}}, 1000, {0, 0, 0}};
	double		ry = kf_replay_us(prior, &typical) > 1 ? kf_replay_us(prior, &typical) : 1,
				sy = kf_restore_us(prior, &tk) > 1 ? kf_restore_us(prior, &tk) : 1;
	double		rq = sqrt_(ry),
				sq = sqrt_(sy);
	double		rs[4] = {1 / rq, 1000 / rq, 500 / rq, 500 / rq};
	double		ss[2] = {1 / sq, 1000 / sq};

	memset(p, 0, sizeof(*p));
	p->p.ops = &cal_ops;
	p->cal = *cal;
	p->c = *prior;
	fit_init(&p->replay, 4, rt, rs);
	fit_init(&p->restore, 2, st, ss);
	return &p->p;
}
