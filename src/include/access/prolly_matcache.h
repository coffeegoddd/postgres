/*-------------------------------------------------------------------------
 *
 * prolly_matcache.h
 *	  Materialization cache interface: the local, physical states of
 *	  commits kept after the sessions that used them have left, or as
 *	  branches move on.
 *
 * A materialization is a commit's canonical state made Postgres's: its
 * tables' heap pages, indexes and TOAST tables, held by a branch's copies.
 * Making one costs a copy of the catalogs and a move of every differing
 * table's rows.  A kept one is immutable: its working set is its commit's
 * root, which nothing writes again, so its relations can be shared with
 * other branches (block-mapped storage shares a relation's map root in
 * O(1) of its size) wherever a move or a first use would otherwise write
 * the same rows again.  A move gives a table whose rows differ from its
 * target's the storage of a kept state whose root holds the target's
 * table: the same data tree, definition, relations and physical layout.
 *
 * A kept state is of one of two kinds, and either can serve a checkout, a
 * reset or a first use, the move planner costing both alike:
 * - a kept checkout: a branch that no transaction writes again, its
 *   working set its head commit's root; a table is given its storage
 *   (branch operations' share());
 * - a kept commit: the pages a branch keeps as of one of the last commits
 *   it was at (prolly.kept_commits, branch operations' keep()), each
 *   table's relations by a snapshot of their storage, however the branch
 *   has moved on since; a table is given storage made of its snapshots
 *   (branch operations' share_kept(), or a first use's make_kept()).
 *
 * Contract:
 * - visit() visits the kept states whose making the snapshot sees
 *   committed (a kept checkout's branch, a kept commit's keeping
 *   transaction: so every tuple their pages hold is one the snapshot sees
 *   as committed or aborted) and whose commit's ordering dependencies
 *   match the installed extensions (the indexes on their pages were built
 *   under them): the session branch's kept commits too, but not the
 *   session's own branch as a kept checkout.  A visited state can be let
 *   go of by another session at any time; its user holds it first
 *   (share() pins the branch; share_kept() and make_kept() lock the
 *   relations against letting go of the pages) and skips it if it is
 *   gone.
 * - kept() says whether the branch called name is a kept checkout.
 * - note_use() is called when a session checks out a kept checkout;
 *   evict() is given the kept checkouts other than the session's, and lets
 *   go of the
 *   idle ones (no session on them) past the budget, least recently used
 *   first, by calling drop() on each until within it: drop() deletes the
 *   branch, true if it is gone (deleted now, or by another meanwhile),
 *   false if it couldn't be (a session came to it), and then the next is
 *   tried.
 * - Every call is in a transaction; nothing kept is changed but through
 *   the branch operations, so what is kept commits or rolls back with it.
 *
 * src/include/access/prolly_matcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_MATCACHE_H
#define PROLLY_MATCACHE_H

#include "access/prolly_refs.h"
#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "nodes/pg_list.h"
#include "utils/snapshot.h"

typedef enum ProllyKeptKind
{
	PROLLY_KEPT_CHECKOUT,
	PROLLY_KEPT_COMMIT,
} ProllyKeptKind;

/* A kept materialization. */
typedef struct ProllyKeptState
{
	ProllyKeptKind kind;
	const char *name;			/* the branch holding it */
	Oid			branch;			/* and its OID */
	ProllyHash	commit;			/* the commit it is of */
	ProllyNodeRef root;			/* that commit's canonical root */
	const ProllyKeptCommit *kept;	/* a kept commit's pages (else NULL) */
} ProllyKeptState;

typedef void (*ProllyKeptVisit) (const ProllyKeptState *state, void *arg);

typedef struct ProllyMatCacheRoutine
{
	ProllyImplInfo info;

	bool		(*kept) (const char *name);
	void		(*visit) (Snapshot snap, ProllyKeptVisit visit, void *arg);
	void		(*note_use) (const char *name);
	void		(*evict) (List *names, bool (*drop) (const char *name, void *arg),
						  void *arg);

	/* shared memory, as prolly_shmem.c reserves it */
	Size		(*shmem_size) (void);
	void		(*shmem_init) (void *area);
} ProllyMatCacheRoutine;

#define PROLLY_MATCACHE_CALLBACK_NAMES \
	"kept", "visit", "note_use", "evict", "shmem_size", "shmem_init"
#define PROLLY_MATCACHE_FIRST_CALLBACK kept
#define PROLLY_MATCACHE_LAST_CALLBACK shmem_init

/* implementation "branches": how many idle kept states it keeps */
extern PGDLLIMPORT int prolly_kept_checkouts;

/*
 * Let go of every kept state of the database no session is on (DROP
 * ROLE's, when something depends on the role: a kept state's objects would
 * otherwise keep a role they name from being dropped, as any branch's do):
 * false if it has none, so nothing changed.  Another database's kept
 * states are let go of only from that database; prolly_kept_states_named()
 * says whether a dependency report (checkSharedDependencies()'s) names
 * objects that may be some.
 */
extern bool prolly_let_go_of_kept_states(void);
extern bool prolly_kept_states_named(const char *detail);

#endif							/* PROLLY_MATCACHE_H */
