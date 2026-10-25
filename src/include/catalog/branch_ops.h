/*-------------------------------------------------------------------------
 *
 * branch_ops.h
 *	  Branch operations: making, deleting, checking out and listing a
 *	  database's branches.
 *
 * A branch has its own copy of every versioned object (one in a versioned
 * schema).  How the copies are made and kept is the implementation's
 * business: the one in branch_copies.c copies the source branch's catalog
 * rows and shares its relations' pages until either branch changes them.
 *
 * Contract:
 * - create and delete are transactional with the calling transaction: an
 *   aborted creation leaves no branch, an aborted deletion the branch.
 * - checkout changes the session's branch at once, and lasts until the
 *   next checkout or the end of the session.
 * - main can't be deleted, nor a branch any session is on.
 *
 * src/include/catalog/branch_ops.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BRANCH_OPS_H
#define BRANCH_OPS_H

#include "common/relpath.h"
#include "datatype/timestamp.h"

/* Visits one branch, by name. */
typedef void (*BranchVisit) (const char *name, Oid branch, void *arg);

/*
 * A relation's pages as keep() kept them, for restore() to give back: the
 * storage that keeps them (its tablespace, 0 for the database's, and file
 * number), the snapshot's id there, and the relation's oldest unfrozen
 * transaction and multixact ids then (its pages hold none older).
 */
typedef struct BranchKeptRelation
{
	Oid			relid;
	Oid			tablespace;
	RelFileNumber relfilenumber;
	uint64		snapshot;
	TransactionId frozenxid;
	MultiXactId minmxid;
} BranchKeptRelation;

/* A table's relations as kept, its own first. */
typedef struct BranchKeptTable
{
	int			nrels;
	const BranchKeptRelation *rels;
} BranchKeptTable;

typedef struct BranchOpsRoutine
{
	const char *name;

	/*
	 * Make a branch called name, a copy of the branch start_point (NULL: the
	 * session's), and return its id.
	 */
	Oid			(*create) (const char *name, const char *start_point);

	/* Delete a branch with its copies. */
	void		(*drop) (const char *name);

	/*
	 * Call branch name newname, as deleting it and making one of the new
	 * name would check: refused if another session is on it, unless force
	 * (sessions on it stay on it, under its new name).
	 */
	void		(*rename) (const char *name, const char *newname, bool force);

	/*
	 * What create and drop check of the session, alone (a branch's name,
	 * the privileges they need, a transaction that can write): refused if
	 * the session may not make, or delete, a branch called name.
	 */
	void		(*check_create) (const char *name);
	void		(*check_drop) (const char *name);

	/* Whether check_create would take name as a branch's (its rules alone). */
	bool		(*name_ok) (const char *name);

	/*
	 * Make a branch as create does, without its checks of the session: the
	 * copies of one that exists already in all but them, which its first
	 * use makes.  The copies of the tables emptied names (qualified names,
	 * as the source branch has them; a name it hasn't, or of no plain
	 * table, passed over) begin empty instead, as TRUNCATE leaves a table:
	 * the table and its TOAST table given new storage, no page shared, its
	 * indexes built over no rows, and its change log told it was emptied.
	 * nowait: if a writer of start_point's is in the middle of changing it,
	 * nothing is made and InvalidOid returned, rather than waiting for the
	 * writer (which may be idle in its transaction for as long as it likes).
	 */
	Oid			(*make) (const char *name, const char *start_point,
						 List *emptied, bool nowait);

	/* Make name the session's branch. */
	void		(*checkout) (const char *name);

	/* The session's branch's name (palloc'd). */
	char	   *(*current) (void);

	/* Visit every branch. */
	void		(*branches) (BranchVisit visit, void *arg);

	/*
	 * Run fn with the session on branch name, back on its own branch after,
	 * whatever happens.
	 */
	void		(*run_on) (const char *name, void (*fn) (void *arg), void *arg);

	/*
	 * Optional: give each of the session branch's relations relids storage
	 * shared with branch source's relation of the same id, as a copy has
	 * it when made (its pages, its maps, how old its oldest row is), the
	 * storage it had going when the transaction commits.  Each is locked
	 * first, without waiting, as TRUNCATE locks it, and the source's as a
	 * copy locks them (against writers); then unchanged(arg), if given, says
	 * whether the source's are still as the caller found them.  The source
	 * is held against deletion to the transaction's end.  False, with
	 * nothing shared, if the source is being deleted, a lock isn't free,
	 * unchanged says no, or one isn't on both branches alike (the same
	 * kind, access method, columns as stored, TOAST table, and for an
	 * index the same definition) or can't be shared (the locks taken are
	 * kept).
	 * Like TRUNCATE, it isn't MVCC-safe: a snapshot taken before it reads
	 * the new storage once it commits.
	 *
	 * A lock held by a page sweep (bmap_compaction_let_go()) is waited
	 * for, here and in restore() and share_kept(), the sweep asked to let
	 * go: within lock_timeout of *wait_start, the operation's (0 until one
	 * of its tries first waits, which sets it), so that lock_timeout bounds
	 * the waits of all the calls one operation makes.
	 */
	bool		(*share) (Oid source, List *relids,
						  bool (*unchanged) (void *arg), void *arg,
						  TimestampTz *wait_start);

	/*
	 * Optional: keep the pages of the session branch's relations relids as
	 * they are, each in a snapshot of its storage that later changes don't
	 * touch, into kept[] (one for each, in order): O(1) in each relation's
	 * size.  A relation whose pages are still those of a snapshot in
	 * prior[] (nprior of them, of its storage) is kept by that one again;
	 * only the others take a snapshot.  Every one of relids is locked
	 * against writers first, without waiting (and let go after); then
	 * settled(arg), if given, says whether their pages hold exactly what
	 * the caller found them to.  False, nothing kept, if a lock isn't free,
	 * settled says no, or one can't be kept (its storage can't, or keeps
	 * too many snapshots already).  Snapshots taken by a transaction that
	 * aborts (or a subtransaction) are let go of later.  Once they are
	 * kept, the snapshots release[] (nrelease of them) are let go of, the
	 * locks still held: a branch being made of kept pages (make_kept())
	 * holds the relations against that while it reads them.
	 */
	bool		(*keep) (List *relids, const BranchKeptRelation *prior,
						 int nprior, BranchKeptRelation *kept,
						 const BranchKeptRelation *release, int nrelease,
						 bool (*settled) (void *arg), void *arg);

	/*
	 * Optional: give the session branch's relations kept[] (nkept of them,
	 * a table's together) back the pages keep() kept, the storage they had
	 * going when the transaction commits, into renewed[] (each one kept as
	 * it is now, by a snapshot of its new storage or, all unchanged since,
	 * the same one).  O(1) in each relation's size, and nothing if none
	 * changed since: if one did, each gets new storage made of its
	 * snapshot (with no free space or visibility map), its row the oldest
	 * transaction ids it had then (one unchanged may hold hints, in its
	 * buffers, true of it alone).  All or none: false, nothing changed,
	 * if one isn't on the branch with the storage that kept it, its
	 * snapshot is gone, its rows' transaction ids are older than the
	 * database still knows, or one that changed is in use (each is locked
	 * as TRUNCATE locks it, without waiting, and refused, as TRUNCATE
	 * refuses, while the session itself has it open or an AFTER trigger's
	 * event queued on it; the locks taken are kept).
	 * Like TRUNCATE, it isn't MVCC-safe: a snapshot taken before it reads
	 * the restored storage once it commits.
	 */
	bool		(*restore) (const BranchKeptRelation *kept, int nkept,
							BranchKeptRelation *renewed,
							TimestampTz *wait_start);

	/*
	 * Optional: let go of a snapshot keep() or restore() took, if its
	 * storage still has it (a dropped relation's went with it).
	 */
	void		(*let_go) (const BranchKeptRelation *kept);

	/*
	 * Optional: make a branch called name as make() does, a copy of branch
	 * source, but each table of tables[] (ntables of them, each its
	 * relations as keep() kept them) whose pages are still kept made of
	 * them, used[] saying which were: O(1) in each relation's size, with
	 * no lock that holds up the source's writers (only definition changes,
	 * VACUUM and letting go of kept pages wait).  The source's other
	 * relations are copied as make() copies them, locked against writers,
	 * the tables of emptied among them begun empty; nowait as for make()
	 * (InvalidOid, nothing made, if a lock is held).
	 */
	Oid			(*make_kept) (const char *name, const char *source,
							  const BranchKeptTable *tables, int ntables,
							  bool *used, List *emptied, bool nowait);

	/*
	 * Optional: give the session branch's relations kept[] (nkept of them,
	 * a table's together) new storage made of the pages keep() kept of
	 * branch source's (the session's own, or another's), O(1) each, as
	 * restore() gives them, whatever source's working set is now.  Each
	 * must be stored alike on both branches, as share() requires, with
	 * source's still on the storage that kept it, its snapshot there and
	 * its rows' transaction ids ones the database still knows.  The source
	 * is held against deletion and its relations against letting go of the
	 * pages (ShareUpdateExclusiveLock), the session's locked as TRUNCATE
	 * locks them (and refused while the session has one open, as restore()
	 * refuses), each tried without waiting.  All or none: false, nothing
	 * changed, otherwise (the locks taken are kept).  Not MVCC-safe: a
	 * snapshot taken before the keeping transaction ended could read
	 * versions source made and deleted, rows never the session branch's,
	 * so the caller gives only pages kept before every snapshot there is.
	 */
	bool		(*share_kept) (Oid source, const BranchKeptRelation *kept,
							   int nkept, TimestampTz *wait_start);
} BranchOpsRoutine;

/* the branch operations of this database */
extern const BranchOpsRoutine *branch_ops(void);

#endif							/* BRANCH_OPS_H */
