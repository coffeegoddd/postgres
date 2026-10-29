/*
 * kfplan.h
 *	  Keyframe planning: when to capture a keyframe (a commit image), and
 *	  how to check out a commit, by replaying rows or by restoring a
 *	  keyframe's pages and replaying the rest.
 *
 * Moving the working set from where it is to a target commit can go:
 *	replay		write the rows that differ (the prolly diff), or
 *	restore		write the pages that differ from a keyframe K (the image
 *				diff and the pages changed since the working set's base),
 *				then replay the rows that differ between K and the target.
 * Both write the pages that differ.  A restore logs every page whole; a
 * replay logs a page whole only the first time it touches it after a
 * checkpoint ("cold"), and otherwise logs just the change ("warm").
 * Replay also pays per row; restore pays a fixed cost for its lock and
 * truncation.  So, as measured (bench/kfbench):
 *	replay(m)	= F_replay + a rows + b_cold cold_pages + b_warm warm_pages
 *	restore(p)	= F_restore + c pages
 * and restore wins once the change is big enough, sooner when its pages
 * are cold.  WAL compression scales the page costs.
 *
 * The planner decides from counts the engine has before writing anything:
 * rows from the prolly diff, pages from the image diff and the WAL, and
 * which of those pages were written since the last checkpoint.
 *
 * A keyframe is worth capturing once the change since the last one would
 * be cheaper to restore than to replay: capturing costs the pages it reads,
 * and every checkout landing near it saves the difference.
 *
 * Costs differ by server, schema and wal_compression, so a planner can
 * calibrate itself: each move's actual time is observed and the costs are
 * refitted, starting from measured ones.
 */
#ifndef KFPLAN_H
#define KFPLAN_H

#include "phash.h"

/* What moving the working set costs, in microseconds. */
typedef struct KfCosts
{
	double		replay_fixed_us;	/* per replay: the transaction */
	double		row_us;			/* replaying one row */
	double		replay_page_us;	/* each cold page a replay dirties */
	double		replay_warm_page_us;	/* each page already written since a checkpoint */
	double		restore_fixed_us;	/* per restore: lock, truncation, transaction */
	double		page_us;		/* restoring one page */
	double		capture_page_us;	/* reading one page into a keyframe */
} KfCosts;

/* Rows to replay, the pages they are on, and how many of those are warm. */
typedef struct KfMove
{
	uint64_t	rows;
	uint64_t	pages;
	uint64_t	warm_pages;		/* <= pages: written since the last checkpoint */
} KfMove;

/* A keyframe a checkout could start from. */
typedef struct KfCandidate
{
	PHash		keyframe;		/* the commit it images */
	uint64_t	pages;			/* pages differing between it and the working set */
	KfMove		after;			/* the rows from it to the target */
} KfCandidate;

typedef enum KfPath
{
	KF_REPLAY,					/* rows only */
	KF_RESTORE					/* a keyframe's pages, then its rows after */
} KfPath;

typedef struct KfChoice
{
	KfPath		path;
	size_t		candidate;		/* with KF_RESTORE: which */
	double		est_us;			/* the chosen path's estimate */
	double		replay_us;		/* replay's estimate, for comparison */
	bool		explored;		/* not the cheapest estimate: taken to learn */
} KfChoice;

typedef struct KfPlanner KfPlanner;

typedef struct KfPlannerOps
{
	/* The cheapest way to the target: replay `m`, or restore a candidate. */
	KfChoice	(*choose) (KfPlanner *p, const KfMove *m, const KfCandidate *c, size_t n);

	/*
	 * Whether to capture a keyframe at a commit `since` past the last one
	 * (its pages are those changed since, which capturing reads).
	 */
	bool		(*should_capture) (KfPlanner *p, const KfMove *since);

	/* What a replay of m, or a restore of pages, actually took. */
	void		(*observe_replay) (KfPlanner *p, const KfMove *m, double us);
	void		(*observe_restore) (KfPlanner *p, uint64_t pages, double us);

	/* The costs it prices with now. */
	void		(*costs) (KfPlanner *p, KfCosts *out);
	void		(*destroy) (KfPlanner *p);
} KfPlannerOps;

struct KfPlanner
{
	const KfPlannerOps *ops;
};

/* Prices every path with fixed costs; observations are ignored. */
extern KfPlanner *kf_cost_planner(const KfCosts *costs);

/*
 * Starts from prior costs and refits them from what it observes: a least
 * squares fit of each path's model, with older observations decaying by
 * `forget` per observation of that path, pulled toward the prior as if it
 * were `prior_weight` observations, and no cost below zero.  It learns
 * only from the paths it takes, so it explores: a path not taken for
 * `explore_every` decisions is taken on the next small move (under a
 * quarter of the running average), which costs little and says the most
 * about fixed costs, so even
 * a prior wrong enough to hide a path cannot keep it from being learned.
 */
typedef struct KfCalibration
{
	double		forget;			/* 0 < forget <= 1; e.g. 0.99 */
	double		prior_weight;	/* e.g. 3 */
	int			explore_every;	/* e.g. 50; 0: no starved-path exploring */
} KfCalibration;

extern KfPlanner *kf_calibrating_planner(const KfCosts *prior, const KfCalibration *cal);

/* Replay's and restore's estimates under fixed costs. */
extern double kf_replay_us(const KfCosts *c, const KfMove *m);
extern double kf_restore_us(const KfCosts *c, const KfCandidate *k);

/*
 * The row count past which restoring beats replaying a cold change spread
 * at pages_per_row, or -1 if it never does.
 */
extern double kf_break_even_rows(const KfCosts *costs, double pages_per_row);

#endif							/* KFPLAN_H */
