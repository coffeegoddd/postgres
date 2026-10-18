/*-------------------------------------------------------------------------
 *
 * prolly_autovacuum.h
 *	  Prolly's stores reclaimed by autovacuum: the chunk store collected,
 *	  its journal moved, and the page stores swept, when due.
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

/* Define the settings (once per process). */
extern void prolly_autovacuum_init(void);

/*
 * An autovacuum worker's work on its database's stores, after its tables:
 * each in a transaction of its own, an error reported and passed over.
 */
extern void prolly_autovacuum(void);

/* The current database's page stores swept (due_only: those due). */
extern void prolly_sweep_page_stores(bool due_only, BmapSweep *total);

#endif							/* PROLLY_AUTOVACUUM_H */
