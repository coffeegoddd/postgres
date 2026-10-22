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

/* prolly_fold_worker, prolly_fold_worker_max_workers, _naptime (ms) */
extern PGDLLIMPORT bool prolly_fold_worker;
extern PGDLLIMPORT int prolly_fold_worker_max_workers;
extern PGDLLIMPORT int prolly_fold_worker_naptime;

extern void ProllyFolderRegister(void);
pg_noreturn extern void ProllyFolderLauncherMain(Datum main_arg);
pg_noreturn extern void ProllyFolderWorkerMain(Datum main_arg);

#endif							/* PROLLY_FOLDER_H */
