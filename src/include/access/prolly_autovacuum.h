/*-------------------------------------------------------------------------
 *
 * prolly_autovacuum.h
 *	  Prolly's stores reclaimed by autovacuum: the chunk store collected,
 *	  its journal moved, the page stores' release queues drained, and the
 *	  page stores swept, when due.
 *
 * src/include/access/prolly_autovacuum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_AUTOVACUUM_H
#define PROLLY_AUTOVACUUM_H

#include "storage/bmap.h"

/* prolly.gc_journal_threshold (MB; -1: no automatic collection) */
extern PGDLLIMPORT int prolly_gc_journal_threshold;
/* prolly.gc_growth_scale_factor */
extern PGDLLIMPORT double prolly_gc_growth_scale_factor;
/* prolly.journal_move_threshold (MB; -1: the journal is never moved) */
extern PGDLLIMPORT int prolly_journal_move_threshold;
extern PGDLLIMPORT int prolly_conjoin_max_files;
/* prolly.autofold_threshold (a fraction of a table's size; 0: off) */
extern PGDLLIMPORT double prolly_autofold_threshold;
/* prolly.release_max_per_pass: release queue entries a pass lets go of */
extern PGDLLIMPORT int prolly_release_max_per_pass;
/* prolly.gc_file_size (kB): how large a collection's files grow */
extern PGDLLIMPORT int prolly_gc_file_size;
/*
 * prolly.log_copied_bytes: a store's copies of its own chunks WAL-log their
 * bytes, not where they come from (for debugging, and comparing the two)
 */
extern PGDLLIMPORT bool prolly_log_copied_bytes;

/* Define the settings (once per process). */
extern void prolly_autovacuum_init(void);

/*
 * An autovacuum worker's work on its database's stores, after its tables:
 * each in a transaction of its own, an error reported and passed over.
 */
extern void prolly_autovacuum(void);

/* The current database's release queues drained, a pass's worth. */
extern void prolly_release_page_stores(BmapRelease *total);

/* The current database's page stores swept (due_only: those due). */
extern void prolly_sweep_page_stores(bool due_only, BmapSweep *total);

#endif							/* PROLLY_AUTOVACUUM_H */
