/*-------------------------------------------------------------------------
 *
 * prolly_folder.h
 *	  The fold worker: branches' working sets folded in the background.
 *
 * src/include/access/prolly_folder.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_FOLDER_H
#define PROLLY_FOLDER_H

/*
 * prolly_fold_worker, prolly_fold_worker_max_workers, _naptime (ms), and
 * when a branch's fold is due: _batch_rows, _max_delay (ms)
 */
extern PGDLLIMPORT bool prolly_fold_worker;
extern PGDLLIMPORT int prolly_fold_worker_max_workers;
extern PGDLLIMPORT int prolly_fold_worker_naptime;
extern PGDLLIMPORT int prolly_fold_worker_batch_rows;
extern PGDLLIMPORT int prolly_fold_worker_max_delay;

/*
 * Pacing (prolly_fold_worker_cost_delay, ms, 0 off; _cost_limit, -1 for
 * vacuum_cost_limit): the fold worker's folds under the cost-based delay
 * vacuum's are.  While a fold of the worker's is paced, the pages it reads
 * and dirties are charged as vacuum's are, and prolly_fold_delay_point(),
 * called in the folds' loops, charges the tree nodes read and written
 * since its last call, then naps once the fold has spent its limit; or,
 * if another backend waits for a lock the fold's holds (its virtual
 * transaction's among them), gives up the fold
 * (an error the worker takes as a retry).  It does nothing in any other
 * fold.
 */
extern PGDLLIMPORT double prolly_fold_worker_cost_delay;
extern PGDLLIMPORT int prolly_fold_worker_cost_limit;
extern PGDLLIMPORT bool prolly_fold_paced;

extern void prolly_fold_delay_point_paced(void);

static inline void
prolly_fold_delay_point(void)
{
	if (unlikely(prolly_fold_paced))
		prolly_fold_delay_point_paced();
}

extern void ProllyFolderRegister(void);
pg_noreturn extern void ProllyFolderLauncherMain(Datum main_arg);
pg_noreturn extern void ProllyFolderWorkerMain(Datum main_arg);

#endif							/* PROLLY_FOLDER_H */
