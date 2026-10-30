/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  public interface routines to storage manager switch.
 *
 * All file system operations on relations dispatch through these routines.
 * An SMgrRelation represents physical on-disk relation files that are open
 * for reading and writing.
 *
 * When a relation is first accessed through the relation cache, the
 * corresponding SMgrRelation entry is opened by calling smgropen(), and the
 * reference is stored in the relation cache entry.
 *
 * Accesses that don't go through the relation cache open the SMgrRelation
 * directly.  That includes flushing buffers from the buffer cache, as well as
 * all accesses in auxiliary processes like the checkpointer or the WAL redo
 * in the startup process.
 *
 * Operations like CREATE, DROP, ALTER TABLE also hold SMgrRelation references
 * independent of the relation cache.  They need to prepare the physical files
 * before updating the relation cache.
 *
 * There is a hash table that holds all the SMgrRelation entries in the
 * backend.  If you call smgropen() twice for the same rel locator, you get a
 * reference to the same SMgrRelation. The reference is valid until the end of
 * transaction.  This makes repeated access to the same relation efficient,
 * and allows caching things like the relation size in the SMgrRelation entry.
 *
 * At end of transaction, all SMgrRelation entries that haven't been pinned
 * are removed.  An SMgrRelation can hold kernel file system descriptors for
 * the underlying files, and we'd like to close those reasonably soon if the
 * file gets deleted.  The SMgrRelations references held by the relcache are
 * pinned to prevent them from being closed.
 *
 * There is another mechanism to close file descriptors early:
 * PROCSIGNAL_BARRIER_SMGRRELEASE.  It is a request to immediately close all
 * file descriptors.  Upon receiving that signal, the backend closes all file
 * descriptors held open by SMgrRelations, but because it can happen in the
 * middle of a transaction, we cannot destroy the SMgrRelation objects
 * themselves, as there could pointers to them in active use.  See
 * smgrrelease() and smgrreleaseall().
 *
 * NB: We need to hold interrupts across most of the functions in this file,
 * as otherwise interrupt processing, e.g. due to a < ERROR elog/ereport, can
 * trigger procsignal processing, which in turn can trigger
 * smgrreleaseall(). Most of the relevant code is not reentrant.  It seems
 * better to put the HOLD_INTERRUPTS()/RESUME_INTERRUPTS() here, instead of
 * trying to push them down to md.c where possible: For one, every smgr
 * implementation would be vulnerable, for another, a good bit of smgr.c code
 * itself is affected too.  Eventually we might want a more targeted solution,
 * allowing e.g. a networked smgr implementation to be interrupted, but many
 * other, more complicated, problems would need to be fixed for that to be
 * viable (e.g. smgr.c is often called with interrupts already held).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/smgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogutils.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "storage/aio.h"
#include "storage/bmap.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/inval.h"


/*
 * This struct of function pointers defines the API between smgr.c and
 * any individual storage manager module.  Note that smgr subfunctions are
 * generally expected to report problems via elog(ERROR).  An exception is
 * that smgr_unlink should use elog(WARNING), rather than erroring out,
 * because we normally unlink relations during post-commit/abort cleanup,
 * and so it's too late to raise an error.  Also, various conditions that
 * would normally be errors should be allowed during bootstrap and/or WAL
 * recovery --- see comments in md.c for details.
 */
typedef struct f_smgr
{
	void		(*smgr_init) (void);	/* may be NULL */
	void		(*smgr_shutdown) (void);	/* may be NULL */

	/*
	 * Does this implementation hold the relation's storage?  Asked by
	 * smgropen() of every implementation but md.c, which holds whatever no
	 * other one claims.  May be NULL.
	 */
	bool		(*smgr_claims) (RelFileLocatorBackend rlocator);
	void		(*smgr_open) (SMgrRelation reln);
	void		(*smgr_close) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_create) (SMgrRelation reln, ForkNumber forknum,
								bool isRedo);
	bool		(*smgr_exists) (SMgrRelation reln, ForkNumber forknum);

	/*
	 * Create dst's main fork as a copy of src's that shares its pages until
	 * either changes them; may be NULL.
	 */
	void		(*smgr_share) (SMgrRelation src, SMgrRelation dst);

	/*
	 * Make durable what this transaction's shares left to be (at commit), or
	 * forget it (!isCommit); may be NULL (a share is durable once made).
	 */
	void		(*smgr_syncshares) (bool isCommit);

	/*
	 * Can the fork's blocks be given other storage, which may have held
	 * another relation's pages?  May be NULL (never).  A fork whose blocks
	 * can be is one whose pages are prepared before they change
	 * (smgr_preparesforchanges): redo tells such pages by their buffers.
	 */
	bool		(*smgr_replacesblocks) (SMgrRelation reln, ForkNumber forknum);

	/*
	 * In recovery: was the block given other storage after WAL position
	 * lsn, so that a record from before then doesn't apply to what it has
	 * now?  Required if smgr_replacesblocks is set.
	 */
	bool		(*smgr_replacedafter) (SMgrRelation reln, ForkNumber forknum,
									   BlockNumber blocknum, XLogRecPtr lsn);

	/*
	 * In recovery, before a full-page image of the block from the record at
	 * WAL position lsn is restored over the version the block has, which may
	 * be newer.  May be NULL.
	 */
	void		(*smgr_prerestore) (SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum, XLogRecPtr lsn);

	/*
	 * Must a fork's pages be prepared before they change (they may be
	 * shared)?  Then the buffer manager calls smgr_prepareforchange once a
	 * page is locked exclusively (or, under a cleanup lock taken without
	 * preparing, when the holder is about to change it), and
	 * smgr_pageisshared answers whether the page is another relation's too,
	 * and whether preparing it would copy it, which it may do for reasons
	 * of its own.  May be NULL.
	 */
	bool		(*smgr_preparesforchanges) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_prepareforchange) (SMgrRelation reln, ForkNumber forknum,
										  BlockNumber blocknum, Buffer buffer);
	bool		(*smgr_pageisshared) (SMgrRelation reln, ForkNumber forknum,
									  BlockNumber blocknum, bool *copies);

	/*
	 * Of a page smgr_pageisshared says is shared: may another relation hold
	 * it, not only snapshots of this one?  May be NULL (always, if
	 * smgr_pageisshared is set).
	 */
	bool		(*smgr_pagesharedelsewhere) (SMgrRelation reln,
											 ForkNumber forknum,
											 BlockNumber blocknum);
	/* before a relation's buffers are dropped to unlink it; may be NULL */
	void		(*smgr_predrop) (SMgrRelation reln, bool isRedo);

	/*
	 * Before the commit record of a transaction that drops the relation at
	 * commit: what the drop will let go of may be recorded now, as the
	 * transaction's, so that a crash after the commit can't lose it.  May
	 * be NULL.
	 */
	void		(*smgr_precommitdrop) (SMgrRelation reln);

	/*
	 * Once relations' buffers have been dropped to unlink them: every
	 * unlink outside recovery (a subtransaction's abort, COMMIT PREPARED)
	 * and the end of a top-level transaction's drops, at commit or abort.
	 * What smgr_precommitdrop prepared is done with by then; a call with
	 * nothing prepared must do nothing.  May be NULL.
	 */
	void		(*smgr_enddrops) (void);

	/*
	 * Before the fork is extended by nblocks through shared buffers, holding
	 * no heavyweight lock that takes no others (a relation's extension
	 * lock); may be NULL.
	 */
	void		(*smgr_beforeextend) (SMgrRelation reln, ForkNumber forknum,
									  BlockNumber nblocks);

	/*
	 * One step of letting go of what dropped storage still holds in the
	 * relation's store (pages it may share with the relation, which count as
	 * shared until then): how many of what it held were let go of, 0 if
	 * nothing was.  May be NULL.
	 */
	uint32		(*smgr_releasestep) (SMgrRelation reln);

	/*
	 * Before the current database's oldest transaction id advances, which
	 * lets the commit log forget older transactions: make whatever state of
	 * the database's storage names a transaction need the log no more.  In a
	 * transaction.  May be NULL.
	 */
	void		(*smgr_settlexids) (void);

	/*
	 * Background maintenance of the current database's storage, by an
	 * autovacuum worker after it has processed the database's tables: let go
	 * of at most release_max more of what dropped storage holds (-1: no
	 * limit), calling pace (if not NULL) between steps, and run whatever
	 * other upkeep is due, adding what was done to *done.  In a transaction.
	 * May be NULL.
	 */
	void		(*smgr_maintain) (int64 release_max, void (*pace) (void),
								  SMgrMaintenance *done);

	/*
	 * May writing out a page of one of a relation's forks read the
	 * relation's buffers of 'forknum'?  May be NULL (never).
	 */
	bool		(*smgr_writesread) (ForkNumber forknum);

	/*
	 * Does the storage manager read relations whose drop has committed,
	 * until their files are unlinked (a sweep counting what every map of a
	 * store holds)?  Such a read after the relations' buffers were dropped
	 * brings pages back into shared buffers, so they are dropped again once
	 * the files are gone.
	 */
	bool		smgr_readsdropped;

	void		(*smgr_unlink) (RelFileLocatorBackend rlocator, ForkNumber forknum,
								bool isRedo);
	void		(*smgr_extend) (SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum, const void *buffer, bool skipFsync);
	void		(*smgr_zeroextend) (SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum, int nblocks, bool skipFsync);
	/* before WAL-logging blocks that will extend the fork; may be NULL */
	void		(*smgr_preextend) (SMgrRelation reln, ForkNumber forknum,
								   BlockNumber blocknum, int nblocks);
	bool		(*smgr_prefetch) (SMgrRelation reln, ForkNumber forknum,
								  BlockNumber blocknum, int nblocks);
	uint32		(*smgr_maxcombine) (SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum);
	void		(*smgr_readv) (SMgrRelation reln, ForkNumber forknum,
							   BlockNumber blocknum,
							   void **buffers, BlockNumber nblocks);
	/* before an asynchronous read's handle is acquired; may be NULL */
	void		(*smgr_prepareread) (SMgrRelation reln, ForkNumber forknum,
									 BlockNumber blocknum, BlockNumber nblocks);
	void		(*smgr_startreadv) (PgAioHandle *ioh,
									SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum,
									void **buffers, BlockNumber nblocks);
	void		(*smgr_writev) (SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum,
								const void **buffers, BlockNumber nblocks,
								bool skipFsync);
	void		(*smgr_writeback) (SMgrRelation reln, ForkNumber forknum,
								   BlockNumber blocknum, BlockNumber nblocks);
	BlockNumber (*smgr_nblocks) (SMgrRelation reln, ForkNumber forknum);
	/* before the critical section a truncation runs in; may be NULL */
	void		(*smgr_pretruncate) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_truncate) (SMgrRelation reln, ForkNumber forknum,
								  BlockNumber old_blocks, BlockNumber nblocks);
	/* after the critical section a truncation runs in; may be NULL */
	void		(*smgr_posttruncate) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_immedsync) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_registersync) (SMgrRelation reln, ForkNumber forknum);
	int			(*smgr_fd) (SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint32 *off);
} f_smgr;

static const f_smgr smgrsw[] = {
	/* magnetic disk */
	[SMGR_MD] = {
		.smgr_init = mdinit,
		.smgr_shutdown = NULL,
		.smgr_open = mdopen,
		.smgr_close = mdclose,
		.smgr_create = mdcreate,
		.smgr_exists = mdexists,
		.smgr_unlink = mdunlink,
		.smgr_extend = mdextend,
		.smgr_zeroextend = mdzeroextend,
		.smgr_prefetch = mdprefetch,
		.smgr_maxcombine = mdmaxcombine,
		.smgr_readv = mdreadv,
		.smgr_startreadv = mdstartreadv,
		.smgr_writev = mdwritev,
		.smgr_writeback = mdwriteback,
		.smgr_nblocks = mdnblocks,
		.smgr_truncate = mdtruncate,
		.smgr_immedsync = mdimmedsync,
		.smgr_registersync = mdregistersync,
		.smgr_fd = mdfd,
	},
	/* block-mapped */
	[SMGR_BMAP] = {
		.smgr_init = bmapinit,
		.smgr_shutdown = NULL,
		.smgr_claims = bmapclaims,
		.smgr_open = bmapopen,
		.smgr_close = bmapclose,
		.smgr_create = bmapcreate,
		.smgr_exists = bmapexists,
		.smgr_share = bmapshare,
		.smgr_syncshares = bmapsyncshares,
		.smgr_replacesblocks = bmapreplacesblocks,
		.smgr_replacedafter = bmapreplacedafter,
		.smgr_prerestore = bmapprerestore,
		.smgr_preparesforchanges = bmappreparesforchanges,
		.smgr_prepareforchange = bmapprepareforchange,
		.smgr_pageisshared = bmappageisshared,
		.smgr_pagesharedelsewhere = bmappagesharedelsewhere,
		.smgr_predrop = bmappredrop,
		.smgr_precommitdrop = bmapprecommitdrop,
		.smgr_enddrops = bmapenddrops,
		.smgr_beforeextend = bmapbeforeextend,
		.smgr_releasestep = bmapreleasestep,
		.smgr_settlexids = bmapsettlexids,
		.smgr_maintain = bmapmaintain,
		.smgr_writesread = bmapwritesread,
		.smgr_readsdropped = true,
		.smgr_unlink = bmapunlink,
		.smgr_extend = bmapextend,
		.smgr_zeroextend = bmapzeroextend,
		.smgr_preextend = bmappreextend,
		.smgr_prefetch = bmapprefetch,
		.smgr_maxcombine = bmapmaxcombine,
		.smgr_readv = bmapreadv,
		.smgr_prepareread = bmapprepareread,
		.smgr_startreadv = bmapstartreadv,
		.smgr_writev = bmapwritev,
		.smgr_writeback = bmapwriteback,
		.smgr_nblocks = bmapnblocks,
		.smgr_pretruncate = bmappretruncate,
		.smgr_truncate = bmaptruncate,
		.smgr_posttruncate = bmapposttruncate,
		.smgr_immedsync = bmapimmedsync,
		.smgr_registersync = bmapregistersync,
		.smgr_fd = bmapfd,
	}
};

static const int NSmgr = lengthof(smgrsw);

static int	smgr_select(RelFileLocatorBackend rlocator);

/*
 * Each backend has a hashtable that stores all extant SMgrRelation objects.
 * In addition, "unpinned" SMgrRelation objects are chained together in a list.
 */
static HTAB *SMgrRelationHash = NULL;

static dlist_head unpinned_relns;

/* local function prototypes */
static void smgrshutdown(int code, Datum arg);
static void smgrdestroy(SMgrRelation reln);

static void smgr_aio_reopen(PgAioHandle *ioh);
static char *smgr_aio_describe_identity(const PgAioTargetData *sd);


const PgAioTargetInfo aio_smgr_target_info = {
	.name = "smgr",
	.reopen = smgr_aio_reopen,
	.describe_identity = smgr_aio_describe_identity,
};


/*
 * smgrinit(), smgrshutdown() -- Initialize or shut down storage
 *								 managers.
 *
 * Note: smgrinit is called during backend startup (normal or standalone
 * case), *not* during postmaster start.  Therefore, any resources created
 * here or destroyed in smgrshutdown are backend-local.
 */
void
smgrinit(void)
{
	int			i;

	HOLD_INTERRUPTS();

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_init)
			smgrsw[i].smgr_init();
	}

	RESUME_INTERRUPTS();

	/* register the shutdown proc */
	on_proc_exit(smgrshutdown, 0);
}

/*
 * on_proc_exit hook for smgr cleanup during backend shutdown
 */
static void
smgrshutdown(int code, Datum arg)
{
	int			i;

	HOLD_INTERRUPTS();

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_shutdown)
			smgrsw[i].smgr_shutdown();
	}

	RESUME_INTERRUPTS();
}

/*
 * smgropen() -- Return an SMgrRelation object, creating it if need be.
 *
 * In versions of PostgreSQL prior to 17, this function returned an object
 * with no defined lifetime.  Now, however, the object remains valid for the
 * lifetime of the transaction, up to the point where AtEOXact_SMgr() is
 * called, making it much easier for callers to know for how long they can
 * hold on to a pointer to the returned object.  If this function is called
 * outside of a transaction, the object remains valid until smgrdestroy() or
 * smgrdestroyall() is called.  Background processes that use smgr but not
 * transactions typically do this once per checkpoint cycle.
 *
 * This does not attempt to actually open the underlying files.
 */
SMgrRelation
smgropen(RelFileLocator rlocator, ProcNumber backend)
{
	RelFileLocatorBackend brlocator;
	SMgrRelation reln;
	bool		found;

	Assert(RelFileNumberIsValid(rlocator.relNumber));

	HOLD_INTERRUPTS();

	if (SMgrRelationHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocatorBackend);
		ctl.entrysize = sizeof(SMgrRelationData);
		SMgrRelationHash = hash_create("smgr relation table", 400,
									   &ctl, HASH_ELEM | HASH_BLOBS);
		dlist_init(&unpinned_relns);
	}

	/* Look up or create an entry */
	brlocator.locator = rlocator;
	brlocator.backend = backend;
	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  &brlocator,
									  HASH_ENTER, &found);

	/* Initialize it if not present before */
	if (!found)
	{
		/* hash_search already filled in the lookup key */
		reln->smgr_targblock = InvalidBlockNumber;
		for (int i = 0; i <= MAX_FORKNUM; ++i)
			reln->smgr_cached_nblocks[i] = InvalidBlockNumber;
		reln->smgr_which = smgr_select(brlocator);

		/* it is not pinned yet */
		reln->pincount = 0;
		dlist_push_tail(&unpinned_relns, &reln->node);

		/* implementation-specific initialization */
		smgrsw[reln->smgr_which].smgr_open(reln);
	}

	RESUME_INTERRUPTS();

	return reln;
}

/*
 * smgr_select() -- Choose the implementation that holds a relation.
 *
 * The first implementation after md.c that claims the relation, else md.c.
 */
static int
smgr_select(RelFileLocatorBackend rlocator)
{
	for (int i = SMGR_MD + 1; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_claims && smgrsw[i].smgr_claims(rlocator))
			return i;
	}
	return SMGR_MD;
}

/*
 * smgrpin() -- Prevent an SMgrRelation object from being destroyed at end of
 *				transaction
 */
void
smgrpin(SMgrRelation reln)
{
	if (reln->pincount == 0)
		dlist_delete(&reln->node);
	reln->pincount++;
}

/*
 * smgrunpin() -- Allow an SMgrRelation object to be destroyed at end of
 *				  transaction
 *
 * The object remains valid, but if there are no other pins on it, it is moved
 * to the unpinned list where it will be destroyed by AtEOXact_SMgr().
 */
void
smgrunpin(SMgrRelation reln)
{
	Assert(reln->pincount > 0);
	reln->pincount--;
	if (reln->pincount == 0)
		dlist_push_tail(&unpinned_relns, &reln->node);
}

/*
 * smgrdestroy() -- Delete an SMgrRelation object.
 */
static void
smgrdestroy(SMgrRelation reln)
{
	ForkNumber	forknum;

	Assert(reln->pincount == 0);

	HOLD_INTERRUPTS();

	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		smgrsw[reln->smgr_which].smgr_close(reln, forknum);

	dlist_delete(&reln->node);

	if (hash_search(SMgrRelationHash,
					&(reln->smgr_rlocator),
					HASH_REMOVE, NULL) == NULL)
		elog(ERROR, "SMgrRelation hashtable corrupted");

	RESUME_INTERRUPTS();
}

/*
 * smgrrelease() -- Release all resources used by this object.
 *
 * The object remains valid.
 */
void
smgrrelease(SMgrRelation reln)
{
	HOLD_INTERRUPTS();

	for (ForkNumber forknum = 0; forknum <= MAX_FORKNUM; forknum++)
	{
		smgrsw[reln->smgr_which].smgr_close(reln, forknum);
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
	}
	reln->smgr_targblock = InvalidBlockNumber;

	RESUME_INTERRUPTS();
}

/*
 * smgrclose() -- Close an SMgrRelation object.
 *
 * The SMgrRelation reference should not be used after this call.  However,
 * because we don't keep track of the references returned by smgropen(), we
 * don't know if there are other references still pointing to the same object,
 * so we cannot remove the SMgrRelation object yet.  Therefore, this is just a
 * synonym for smgrrelease() at the moment.
 */
void
smgrclose(SMgrRelation reln)
{
	smgrrelease(reln);
}

/*
 * smgrdestroyall() -- Release resources used by all unpinned objects.
 *
 * It must be known that there are no pointers to SMgrRelations, other than
 * those pinned with smgrpin().
 */
void
smgrdestroyall(void)
{
	dlist_mutable_iter iter;

	/* seems unsafe to accept interrupts while in a dlist_foreach_modify() */
	HOLD_INTERRUPTS();

	/*
	 * Zap all unpinned SMgrRelations.  We rely on smgrdestroy() to remove
	 * each one from the list.
	 */
	dlist_foreach_modify(iter, &unpinned_relns)
	{
		SMgrRelation rel = dlist_container(SMgrRelationData, node,
										   iter.cur);

		smgrdestroy(rel);
	}

	RESUME_INTERRUPTS();
}

/*
 * smgrreleaseall() -- Release resources used by all objects.
 */
void
smgrreleaseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	/* seems unsafe to accept interrupts while iterating */
	HOLD_INTERRUPTS();

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
	{
		smgrrelease(reln);
	}

	RESUME_INTERRUPTS();
}

/*
 * smgrreleaserellocator() -- Release resources for given RelFileLocator, if
 *							  it's open.
 *
 * This has the same effects as smgrrelease(smgropen(rlocator)), but avoids
 * uselessly creating a hashtable entry only to drop it again when no
 * such entry exists already.
 */
void
smgrreleaserellocator(RelFileLocatorBackend rlocator)
{
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  &rlocator,
									  HASH_FIND, NULL);
	if (reln != NULL)
		smgrrelease(reln);
}

/*
 * smgrexists() -- Does the underlying file for a fork exist?
 */
bool
smgrexists(SMgrRelation reln, ForkNumber forknum)
{
	bool		ret;

	HOLD_INTERRUPTS();
	ret = smgrsw[reln->smgr_which].smgr_exists(reln, forknum);
	RESUME_INTERRUPTS();

	return ret;
}

/*
 * smgrcreate() -- Create a new relation.
 *
 * Given an already-created (but presumably unused) SMgrRelation,
 * cause the underlying disk file or other storage for the fork
 * to be created.
 */
void
smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_create(reln, forknum, isRedo);
	RESUME_INTERRUPTS();
}

/*
 * smgrcreatewith() -- Create a new relation's fork in a given implementation.
 *
 * Like smgrcreate(), but the storage is created by implementation 'which'
 * (SMGR_*), which holds the relation from then on.  An SMgrRelation opened
 * before the relation existed was given md.c, so it is switched here.
 */
void
smgrcreatewith(SMgrRelation reln, int which, ForkNumber forknum, bool isRedo)
{
	Assert(which >= 0 && which < NSmgr);

	HOLD_INTERRUPTS();
	if (reln->smgr_which != which)
	{
		for (ForkNumber fork = 0; fork <= MAX_FORKNUM; fork++)
		{
			smgrsw[reln->smgr_which].smgr_close(reln, fork);
			reln->smgr_cached_nblocks[fork] = InvalidBlockNumber;
		}
		reln->smgr_which = which;
		smgrsw[which].smgr_open(reln);
	}
	smgrsw[which].smgr_create(reln, forknum, isRedo);
	RESUME_INTERRUPTS();
}

/*
 * smgrsyncshares() -- Make the transaction's shares durable.
 *
 * A share may leave the blocks it shares to be made durable after it, once
 * the caller no longer keeps src from changing; this does it, for every
 * share made since the last call.  It is called at commit, and earlier by a
 * caller that would rather not wait for it there.  At abort (!isCommit)
 * what is left is forgotten: the copies are dropped.
 */
void
smgrsyncshares(bool isCommit)
{
	for (int i = 0; i < NSmgr; i++)
		if (smgrsw[i].smgr_syncshares)
			smgrsw[i].smgr_syncshares(isCommit);
}

/*
 * smgrshare() -- Create dst's main fork as a copy of src's, sharing its pages.
 *
 * dst is a new relation in the same tablespace and database, held by src's
 * implementation from then on.  The pages stay shared until either relation
 * changes one, which gives it its own copy of the page.
 *
 * The caller keeps src's pages from changing meanwhile, and arranges for dst
 * to be dropped if the share fails partway, which may leave it holding some
 * of src's pages.
 */
void
smgrshare(SMgrRelation src, SMgrRelation dst)
{
	int			which = src->smgr_which;

	if (smgrsw[which].smgr_share == NULL)
		elog(ERROR, "storage of relation %s can't be shared",
			 relpath(src->smgr_rlocator, MAIN_FORKNUM).str);

	smgrusewhich(dst, which);
	smgrsw[which].smgr_share(src, dst);
}

/*
 * smgrusewhich() -- Have 'reln', a relation not created yet, held by
 *					 implementation 'which' from now on: one that will make
 *					 its storage itself (a share, its own way).
 */
void
smgrusewhich(SMgrRelation reln, int which)
{
	HOLD_INTERRUPTS();
	if (reln->smgr_which != which)
	{
		for (ForkNumber fork = 0; fork <= MAX_FORKNUM; fork++)
		{
			smgrsw[reln->smgr_which].smgr_close(reln, fork);
			reln->smgr_cached_nblocks[fork] = InvalidBlockNumber;
		}
		reln->smgr_which = which;
		smgrsw[which].smgr_open(reln);
	}
	RESUME_INTERRUPTS();
}

/*
 * smgrreplacedafter() -- In recovery, was the block given other storage
 * after WAL position lsn?
 *
 * Redo skips reading a block for a record from before then: what the record
 * changed is in the page the replacement began with, and the storage the
 * block has now may still hold another relation's page.
 */
bool
smgrreplacedafter(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				  XLogRecPtr lsn)
{
	if (!smgrreplacesblocks(reln, forknum))
		return false;
	return smgrsw[reln->smgr_which].smgr_replacedafter(reln, forknum,
													   blocknum, lsn);
}

/*
 * smgrprerestore() -- In recovery, before a full-page image of the block from
 * the record at WAL position lsn is restored over its current version.
 *
 * The version replaced may be newer than the image, and say something
 * recovery needs to know until replay reaches it.
 */
void
smgrprerestore(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   XLogRecPtr lsn)
{
	if (smgrsw[reln->smgr_which].smgr_prerestore != NULL)
		smgrsw[reln->smgr_which].smgr_prerestore(reln, forknum, blocknum, lsn);
}

/*
 * smgrreplacesblocks() -- Can the fork's blocks be given other storage
 * (smgrreplacedafter()), which may have held another relation's pages?
 */
bool
smgrreplacesblocks(SMgrRelation reln, ForkNumber forknum)
{
	if (smgrsw[reln->smgr_which].smgr_replacesblocks == NULL)
		return false;
	return smgrsw[reln->smgr_which].smgr_replacesblocks(reln, forknum);
}

/*
 * smgrpreparesforchanges() -- Must the fork's pages be prepared before they
 *							   change?
 */
bool
smgrpreparesforchanges(SMgrRelation reln, ForkNumber forknum)
{
	return smgrsw[reln->smgr_which].smgr_preparesforchanges != NULL &&
		smgrsw[reln->smgr_which].smgr_preparesforchanges(reln, forknum);
}

/*
 * smgrprepareforchange() -- Prepare a page, locked exclusively in 'buffer',
 *							 before it changes.
 *
 * A page shared with another relation is copied, so the change is this
 * relation's alone.  Called before any critical section: it may allocate,
 * write and log.
 */
void
smgrprepareforchange(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum, Buffer buffer)
{
	if (smgrsw[reln->smgr_which].smgr_prepareforchange)
		smgrsw[reln->smgr_which].smgr_prepareforchange(reln, forknum, blocknum,
													   buffer);
}

/*
 * smgrpageisshared() -- Is the page another relation's too, so that
 *						 preparing it for a change copies it?
 *
 * *copies says whether preparing it would copy it, for that reason or
 * another.
 */
bool
smgrpageisshared(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				 bool *copies)
{
	*copies = false;
	return smgrsw[reln->smgr_which].smgr_pageisshared != NULL &&
		smgrsw[reln->smgr_which].smgr_pageisshared(reln, forknum, blocknum,
												   copies);
}

/*
 * smgrpagesharedelsewhere() -- Of a shared page: may another relation hold
 *								it, not only the relation's own snapshots?
 *
 * Storage that shares no page (md.c) shares none elsewhere either; storage
 * that shares pages but can't tell its own snapshots from other relations
 * answers yes.
 */
bool
smgrpagesharedelsewhere(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum)
{
	const f_smgr *sw = &smgrsw[reln->smgr_which];

	if (sw->smgr_pageisshared == NULL)
		return false;
	return sw->smgr_pagesharedelsewhere == NULL ||
		sw->smgr_pagesharedelsewhere(reln, forknum, blocknum);
}

/*
 * smgrwritesread() -- May writing out a page of one fork read the buffers of
 *					   'forknum', of the same relation?
 *
 * Asked of every implementation, since a buffer doesn't say which one holds
 * its page.  Whoever drops such buffers while pages they serve may still be
 * written out drops them last.
 */
bool
smgrwritesread(ForkNumber forknum)
{
	for (int i = 0; i < NSmgr; i++)
		if (smgrsw[i].smgr_writesread && smgrsw[i].smgr_writesread(forknum))
			return true;
	return false;
}

/* does this relation's storage implementation share pages (smgrshare())? */
bool
smgrcanshare(SMgrRelation reln)
{
	return smgrsw[reln->smgr_which].smgr_share != NULL;
}

/*
 * smgrprecommitdrop() -- Prepare to drop a relation at commit, before the
 *						  commit record.
 *
 * The transaction may still abort after this, leaving the relation as it
 * is.
 */
void
smgrprecommitdrop(SMgrRelation reln)
{
	if (smgrsw[reln->smgr_which].smgr_precommitdrop != NULL)
		smgrsw[reln->smgr_which].smgr_precommitdrop(reln);
}

/*
 * smgrbeforeextend() -- Prepare to extend a fork by nblocks.
 *
 * Called by the buffer manager before it takes the relation's extension
 * lock.
 */
void
smgrbeforeextend(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks)
{
	if (smgrsw[reln->smgr_which].smgr_beforeextend != NULL)
		smgrsw[reln->smgr_which].smgr_beforeextend(reln, forknum, nblocks);
}

/*
 * smgrenddrops() -- Relations' drops are done (their buffers dropped).
 *
 * Called by every unlink outside recovery, and at the end of a top-level
 * transaction's drops, at commit or abort.
 */
void
smgrenddrops(void)
{
	for (int i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_enddrops)
			smgrsw[i].smgr_enddrops();
	}
}

/*
 * smgrreleasestep() -- Let go of some of what dropped storage still holds
 *						in the relation's store.
 *
 * Returns how much was let go of, 0 if nothing was (or nothing is held).
 * In a transaction.
 */
uint32
smgrreleasestep(SMgrRelation reln)
{
	if (smgrsw[reln->smgr_which].smgr_releasestep == NULL)
		return 0;
	return smgrsw[reln->smgr_which].smgr_releasestep(reln);
}

/*
 * smgrsettlexids() -- Before the current database's oldest transaction id
 *					   advances, have every storage manager settle what of
 *					   its storage names a transaction.
 *
 * The commit log may forget transactions older than the new horizon, so
 * nothing may still need to ask it about them afterwards.
 */
void
smgrsettlexids(void)
{
	for (int i = 0; i < NSmgr; i++)
		if (smgrsw[i].smgr_settlexids)
			smgrsw[i].smgr_settlexids();
}

/*
 * smgrmaintain() -- Background maintenance of the current database's
 *					 storage, in every storage manager that does any.
 *
 * Lets go of at most release_max entries of what dropped storage still
 * holds in all (-1: no limit), calling pace, if not NULL, between steps so
 * that the caller can sleep under its cost limits and accept interrupts,
 * and runs what other upkeep is due.  *done is zeroed, then counts what was
 * done.  In a transaction.
 */
void
smgrmaintain(int64 release_max, void (*pace) (void), SMgrMaintenance *done)
{
	memset(done, 0, sizeof(*done));
	for (int i = 0; i < NSmgr; i++)
	{
		int64		left = release_max;

		if (smgrsw[i].smgr_maintain == NULL)
			continue;
		if (release_max >= 0)
			left = Max(release_max - (int64) done->released, 0);
		smgrsw[i].smgr_maintain(left, pace, done);
	}
}

/*
 * smgrdosyncall() -- Immediately sync all forks of all given relations
 *
 * All forks of all given relations are synced out to the store.
 *
 * This is equivalent to FlushRelationBuffers() for each smgr relation,
 * then calling smgrimmedsync() for all forks of each relation, but it's
 * significantly quicker so should be preferred when possible.
 */
void
smgrdosyncall(SMgrRelation *rels, int nrels)
{
	int			i = 0;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	FlushRelationsAllBuffers(rels, nrels);

	HOLD_INTERRUPTS();

	/*
	 * Sync the physical file(s).
	 */
	for (i = 0; i < nrels; i++)
	{
		int			which = rels[i]->smgr_which;

		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		{
			if (smgrsw[which].smgr_exists(rels[i], forknum))
				smgrsw[which].smgr_immedsync(rels[i], forknum);
		}
	}

	RESUME_INTERRUPTS();
}

/*
 * smgrdounlinkall() -- Immediately unlink all forks of all given relations
 *
 * All forks of all given relations are removed from the store.  This
 * should not be used during transactional operations, since it can't be
 * undone.
 *
 * If isRedo is true, it is okay for the underlying file(s) to be gone
 * already.
 */
void
smgrdounlinkall(SMgrRelation *rels, int nrels, bool isRedo)
{
	int			i = 0;
	RelFileLocatorBackend *rlocators;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	/*
	 * It would be unsafe to process interrupts between DropRelationBuffers()
	 * and unlinking the underlying files. This probably should be a critical
	 * section, but we're not there yet.
	 */
	HOLD_INTERRUPTS();

	/* whatever a storage manager must read before the buffers go */
	for (i = 0; i < nrels; i++)
	{
		int			which = rels[i]->smgr_which;

		if (smgrsw[which].smgr_predrop)
			smgrsw[which].smgr_predrop(rels[i], isRedo);
	}

	/*
	 * Get rid of any remaining buffers for the relations.  bufmgr will just
	 * drop them without bothering to write the contents.
	 */
	DropRelationsAllBuffers(rels, nrels);

	/* (what the storage managers held off until the buffers went, let go) */
	if (!isRedo)
		smgrenddrops();

	/*
	 * create an array which contains all relations to be dropped, and close
	 * each relation's forks at the smgr level while at it
	 */
	rlocators = palloc_array(RelFileLocatorBackend, nrels);
	for (i = 0; i < nrels; i++)
	{
		RelFileLocatorBackend rlocator = rels[i]->smgr_rlocator;
		int			which = rels[i]->smgr_which;

		rlocators[i] = rlocator;

		/* Close the forks at smgr level */
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			smgrsw[which].smgr_close(rels[i], forknum);
	}

	/*
	 * Send a shared-inval message to force other backends to close any
	 * dangling smgr references they may have for these rels.  We should do
	 * this before starting the actual unlinking, in case we fail partway
	 * through that step.  Note that the sinval messages will eventually come
	 * back to this backend, too, and thereby provide a backstop that we
	 * closed our own smgr rel.
	 */
	for (i = 0; i < nrels; i++)
		CacheInvalidateSmgr(rlocators[i]);

	/*
	 * Delete the physical file(s).
	 *
	 * Note: smgr_unlink must treat deletion failure as a WARNING, not an
	 * ERROR, because we've already decided to commit or abort the current
	 * xact.
	 */

	for (i = 0; i < nrels; i++)
	{
		int			which = rels[i]->smgr_which;

		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			smgrsw[which].smgr_unlink(rlocators[i], forknum, isRedo);
	}

	/*
	 * The buffers a storage manager's own readers read back between the drop
	 * of the buffers and the unlink, dropped too.  A reader that reads them
	 * after this finds the files gone, once it is done with them (or
	 * fails), and drops them itself.  (Recovery runs no such readers.)
	 */
	if (!isRedo)
	{
		SMgrRelation *redrop = palloc_array(SMgrRelation, nrels);
		int			nredrop = 0;

		for (i = 0; i < nrels; i++)
			if (smgrsw[rels[i]->smgr_which].smgr_readsdropped)
				redrop[nredrop++] = rels[i];
		if (nredrop > 0)
			DropUnlinkedRelationsAllBuffers(redrop, nredrop);
		pfree(redrop);
	}

	pfree(rlocators);

	RESUME_INTERRUPTS();
}


/*
 * smgrextend() -- Add a new block to a file.
 *
 * The semantics are nearly the same as smgrwrite(): write at the
 * specified position.  However, this is to be used for the case of
 * extending a relation (i.e., blocknum is at or beyond the current
 * EOF).  Note that we assume writing a block beyond current EOF
 * causes intervening file space to become filled with zeroes.
 */
void
smgrextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void *buffer, bool skipFsync)
{
	HOLD_INTERRUPTS();

	smgrsw[reln->smgr_which].smgr_extend(reln, forknum, blocknum,
										 buffer, skipFsync);

	/*
	 * Normally we expect this to increase nblocks by one, but if the cached
	 * value isn't as expected, just invalidate it so the next call asks the
	 * kernel.
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + 1;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;

	RESUME_INTERRUPTS();
}

/*
 * smgrzeroextend() -- Add new zeroed out blocks to a file.
 *
 * Similar to smgrextend(), except the relation can be extended by
 * multiple blocks at once and the added blocks will be filled with
 * zeroes.
 */
void
smgrzeroextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   int nblocks, bool skipFsync)
{
	HOLD_INTERRUPTS();

	smgrsw[reln->smgr_which].smgr_zeroextend(reln, forknum, blocknum,
											 nblocks, skipFsync);

	/*
	 * Normally we expect this to increase the fork size by nblocks, but if
	 * the cached value isn't as expected, just invalidate it so the next call
	 * asks the kernel.
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + nblocks;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;

	RESUME_INTERRUPTS();
}

/*
 * smgrpreextend() -- Prepare to extend a fork with blocks logged first.
 *
 * A caller that WAL-logs new blocks' contents before writing them with
 * smgrextend() calls this before logging them, so a storage manager that
 * must record where a block lives can do so before any record names it.
 * Afterwards the fork may already be 'blocknum + nblocks' long.
 */
void
smgrpreextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  int nblocks)
{
	if (smgrsw[reln->smgr_which].smgr_preextend == NULL)
		return;
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_preextend(reln, forknum, blocknum, nblocks);
	reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
	RESUME_INTERRUPTS();
}

/*
 * smgrprefetch() -- Initiate asynchronous read of the specified block of a relation.
 *
 * In recovery only, this can return false to indicate that a file
 * doesn't exist (presumably it has been dropped by a later WAL
 * record).
 */
bool
smgrprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 int nblocks)
{
	bool		ret;

	HOLD_INTERRUPTS();
	ret = smgrsw[reln->smgr_which].smgr_prefetch(reln, forknum, blocknum, nblocks);
	RESUME_INTERRUPTS();

	return ret;
}

/*
 * smgrmaxcombine() - Return the maximum number of total blocks that can be
 *				 combined with an IO starting at blocknum.
 *
 * The returned value includes the IO for blocknum itself.
 */
uint32
smgrmaxcombine(SMgrRelation reln, ForkNumber forknum,
			   BlockNumber blocknum)
{
	uint32		ret;

	HOLD_INTERRUPTS();
	ret = smgrsw[reln->smgr_which].smgr_maxcombine(reln, forknum, blocknum);
	RESUME_INTERRUPTS();

	return ret;
}

/*
 * smgrreadv() -- read a particular block range from a relation into the
 *				 supplied buffers.
 *
 * This routine is called from the buffer manager in order to
 * instantiate pages in the shared buffer cache.  All storage managers
 * return pages in the format that POSTGRES expects.
 *
 * If more than one block is intended to be read, callers need to use
 * smgrmaxcombine() to check how many blocks can be combined into one IO.
 */
void
smgrreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  void **buffers, BlockNumber nblocks)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_readv(reln, forknum, blocknum, buffers,
										nblocks);
	RESUME_INTERRUPTS();
}

/*
 * smgrprepareread() -- Prepare to start an asynchronous read.
 *
 * Called before the read's I/O handle is acquired; a backend can hold only
 * one handle, so whatever a storage manager must read to start the read
 * (a storage manager that maps blocks to storage of its own reads its
 * map), it reads here.
 */
void
smgrprepareread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				BlockNumber nblocks)
{
	if (smgrsw[reln->smgr_which].smgr_prepareread == NULL)
		return;
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_prepareread(reln, forknum, blocknum,
											  nblocks);
	RESUME_INTERRUPTS();
}

/*
 * smgrstartreadv() -- asynchronous version of smgrreadv()
 *
 * This starts an asynchronous readv IO using the IO handle `ioh`. Other than
 * `ioh` all parameters are the same as smgrreadv().
 *
 * Completion callbacks above smgr will be passed the result as the number of
 * successfully read blocks if the read [partially] succeeds (Buffers for
 * blocks not successfully read might bear unspecified modifications, up to
 * the full nblocks). This maintains the abstraction that smgr operates on the
 * level of blocks, rather than bytes.
 *
 * Compared to smgrreadv(), more responsibilities fall on the caller:
 * - Partial reads need to be handled by the caller re-issuing IO for the
 *   unread blocks
 * - smgr will ereport(LOG_SERVER_ONLY) some problems, but higher layers are
 *   responsible for pgaio_result_report() to mirror that news to the user (if
 *   the IO results in PGAIO_RS_WARNING) or abort the (sub)transaction (if
 *   PGAIO_RS_ERROR).
 * - Under Valgrind, the "buffers" memory may or may not change status to
 *   DEFINED, depending on io_method and concurrent activity.
 */
void
smgrstartreadv(PgAioHandle *ioh,
			   SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   void **buffers, BlockNumber nblocks)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_startreadv(ioh,
											 reln, forknum, blocknum, buffers,
											 nblocks);
	RESUME_INTERRUPTS();
}

/*
 * smgrwritev() -- Write the supplied buffers out.
 *
 * This is to be used only for updating already-existing blocks of a
 * relation (ie, those before the current EOF).  To extend a relation,
 * use smgrextend().
 *
 * This is not a synchronous write -- the block is not necessarily
 * on disk at return, only dumped out to the kernel.  However,
 * provisions will be made to fsync the write before the next checkpoint.
 *
 * NB: The mechanism to ensure fsync at next checkpoint assumes that there is
 * something that prevents a concurrent checkpoint from "racing ahead" of the
 * write.  One way to prevent that is by holding a lock on the buffer; the
 * buffer manager's writes are protected by that.  The bulk writer facility
 * in bulk_write.c checks the redo pointer and calls smgrimmedsync() if a
 * checkpoint happened; that relies on the fact that no other backend can be
 * concurrently modifying the page.
 *
 * skipFsync indicates that the caller will make other provisions to
 * fsync the relation, so we needn't bother.  Temporary relations also
 * do not require fsync.
 *
 * If more than one block is intended to be read, callers need to use
 * smgrmaxcombine() to check how many blocks can be combined into one IO.
 */
void
smgrwritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_writev(reln, forknum, blocknum,
										 buffers, nblocks, skipFsync);
	RESUME_INTERRUPTS();
}

/*
 * smgrwriteback() -- Trigger kernel writeback for the supplied range of
 *					   blocks.
 */
void
smgrwriteback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  BlockNumber nblocks)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_writeback(reln, forknum, blocknum,
											nblocks);
	RESUME_INTERRUPTS();
}

/*
 * smgrnblocks() -- Calculate the number of blocks in the
 *					supplied relation.
 */
BlockNumber
smgrnblocks(SMgrRelation reln, ForkNumber forknum)
{
	BlockNumber result;

	/* Check and return if we get the cached value for the number of blocks. */
	result = smgrnblocks_cached(reln, forknum);
	if (result != InvalidBlockNumber)
		return result;

	HOLD_INTERRUPTS();

	result = smgrsw[reln->smgr_which].smgr_nblocks(reln, forknum);

	reln->smgr_cached_nblocks[forknum] = result;

	RESUME_INTERRUPTS();

	return result;
}

/*
 * smgrnblocks_cached() -- Get the cached number of blocks in the supplied
 *						   relation.
 *
 * Returns an InvalidBlockNumber when not in recovery and when the relation
 * fork size is not cached.
 */
BlockNumber
smgrnblocks_cached(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * For now, this function uses cached values only in recovery due to lack
	 * of a shared invalidation mechanism for changes in file size.  Code
	 * elsewhere reads smgr_cached_nblocks and copes with stale data.
	 */
	if (InRecovery && reln->smgr_cached_nblocks[forknum] != InvalidBlockNumber)
		return reln->smgr_cached_nblocks[forknum];

	return InvalidBlockNumber;
}

/*
 * smgrpretruncate() -- Prepare to truncate the given forks.
 *
 * smgrtruncate() runs in a critical section; anything a storage manager
 * must read to truncate a fork, it reads here, before that section.
 */
void
smgrpretruncate(SMgrRelation reln, ForkNumber *forknum, int nforks)
{
	for (int i = 0; i < nforks; i++)
	{
		if (smgrsw[reln->smgr_which].smgr_pretruncate)
			smgrsw[reln->smgr_which].smgr_pretruncate(reln, forknum[i]);
	}
}

/*
 * smgrposttruncate() -- Finish truncating the given forks.
 *
 * After smgrtruncate()'s critical section: what a storage manager may do
 * once the truncation is done, such as letting go of the storage past the
 * new length.  A crash before it leaves that to the storage manager.
 */
void
smgrposttruncate(SMgrRelation reln, ForkNumber *forknum, int nforks)
{
	for (int i = 0; i < nforks; i++)
	{
		if (smgrsw[reln->smgr_which].smgr_posttruncate)
			smgrsw[reln->smgr_which].smgr_posttruncate(reln, forknum[i]);
	}
}

/*
 * smgrtruncate() -- Truncate the given forks of supplied relation to
 *					 each specified numbers of blocks
 *
 * The truncation is done immediately, so this can't be rolled back.
 *
 * The caller must hold AccessExclusiveLock on the relation, to ensure that
 * other backends receive the smgr invalidation event that this function sends
 * before they access any forks of the relation again.  The current size of
 * the forks should be provided in old_nblocks.  This function should normally
 * be called in a critical section, but the current size must be checked
 * outside the critical section, and no interrupts or smgr functions relating
 * to this relation should be called in between.
 */
void
smgrtruncate(SMgrRelation reln, ForkNumber *forknum, int nforks,
			 BlockNumber *old_nblocks, BlockNumber *nblocks)
{
	int			i;

	/*
	 * Get rid of any buffers for the about-to-be-deleted blocks. bufmgr will
	 * just drop them without bothering to write the contents.
	 */
	DropRelationBuffers(reln, forknum, nforks, nblocks);

	/*
	 * Send a shared-inval message to force other backends to close any smgr
	 * references they may have for this rel.  This is useful because they
	 * might have open file pointers to segments that got removed, and/or
	 * smgr_targblock variables pointing past the new rel end.  (The inval
	 * message will come back to our backend, too, causing a
	 * probably-unnecessary local smgr flush.  But we don't expect that this
	 * is a performance-critical path.)  As in the unlink code, we want to be
	 * sure the message is sent before we start changing things on-disk.
	 */
	CacheInvalidateSmgr(reln->smgr_rlocator);

	/* Do the truncation */
	for (i = 0; i < nforks; i++)
	{
		/* Make the cached size invalid if we encounter an error. */
		reln->smgr_cached_nblocks[forknum[i]] = InvalidBlockNumber;

		smgrsw[reln->smgr_which].smgr_truncate(reln, forknum[i],
											   old_nblocks[i], nblocks[i]);

		/*
		 * We might as well update the local smgr_cached_nblocks values. The
		 * smgr cache inval message that this function sent will cause other
		 * backends to invalidate their copies of smgr_cached_nblocks, and
		 * these ones too at the next command boundary. But ensure they aren't
		 * outright wrong until then.
		 *
		 * We can have nblocks > old_nblocks when a relation was truncated
		 * multiple times, a replica applied all the truncations, and later
		 * restarts from a restartpoint located before the truncations. The
		 * relation on disk will be the size of the last truncate. When
		 * replaying the first truncate, we will have nblocks > current size.
		 * In such cases, smgr_truncate does nothing, so set the cached size
		 * to the old size rather than the requested size.
		 */
		reln->smgr_cached_nblocks[forknum[i]] =
			nblocks[i] > old_nblocks[i] ? old_nblocks[i] : nblocks[i];
	}
}

/*
 * smgrregistersync() -- Request a relation to be sync'd at next checkpoint
 *
 * This can be used after calling smgrwrite() or smgrextend() with skipFsync =
 * true, to register the fsyncs that were skipped earlier.
 *
 * Note: be mindful that a checkpoint could already have happened between the
 * smgrwrite or smgrextend calls and this!  In that case, the checkpoint
 * already missed fsyncing this relation, and you should use smgrimmedsync
 * instead.  Most callers should use the bulk loading facility in bulk_write.c
 * which handles all that.
 */
void
smgrregistersync(SMgrRelation reln, ForkNumber forknum)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_registersync(reln, forknum);
	RESUME_INTERRUPTS();
}

/*
 * smgrimmedsync() -- Force the specified relation to stable storage.
 *
 * Synchronously force all previous writes to the specified relation
 * down to disk.
 *
 * This is useful for building completely new relations (eg, new
 * indexes).  Instead of incrementally WAL-logging the index build
 * steps, we can just write completed index pages to disk with smgrwrite
 * or smgrextend, and then fsync the completed index file before
 * committing the transaction.  (This is sufficient for purposes of
 * crash recovery, since it effectively duplicates forcing a checkpoint
 * for the completed index.  But it is *not* sufficient if one wishes
 * to use the WAL log for PITR or replication purposes: in that case
 * we have to make WAL entries as well.)
 *
 * The preceding writes should specify skipFsync = true to avoid
 * duplicative fsyncs.
 *
 * Note that you need to do FlushRelationBuffers() first if there is
 * any possibility that there are dirty buffers for the relation;
 * otherwise the sync is not very meaningful.
 *
 * Most callers should use the bulk loading facility in bulk_write.c
 * instead of calling this directly.
 */
void
smgrimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	HOLD_INTERRUPTS();
	smgrsw[reln->smgr_which].smgr_immedsync(reln, forknum);
	RESUME_INTERRUPTS();
}

/*
 * Return fd for the specified block number and update *off to the appropriate
 * position.
 *
 * This is only to be used for when AIO needs to perform the IO in a different
 * process than where it was issued (e.g. in an IO worker).
 */
static int
smgrfd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint32 *off)
{
	int			fd;

	/*
	 * The caller needs to prevent interrupts from being processed, otherwise
	 * the FD could be closed prematurely.
	 */
	Assert(!INTERRUPTS_CAN_BE_PROCESSED());

	fd = smgrsw[reln->smgr_which].smgr_fd(reln, forknum, blocknum, off);

	return fd;
}

/*
 * AtEOXact_SMgr
 *
 * This routine is called during transaction commit or abort (it doesn't
 * particularly care which).  All unpinned SMgrRelation objects are destroyed.
 *
 * We do this as a compromise between wanting transient SMgrRelations to
 * live awhile (to amortize the costs of blind writes of multiple blocks)
 * and needing them to not live forever (since we're probably holding open
 * a kernel file descriptor for the underlying file, and we need to ensure
 * that gets closed reasonably soon if the file gets deleted).
 */
void
AtEOXact_SMgr(void)
{
	smgrdestroyall();
}

/*
 * This routine is called when we are ordered to release all open files by a
 * ProcSignalBarrier.
 */
bool
ProcessBarrierSmgrRelease(void)
{
	smgrreleaseall();
	return true;
}

/*
 * Set target of the IO handle to be smgr and initialize all the relevant
 * pieces of data.
 */
void
pgaio_io_set_target_smgr(PgAioHandle *ioh,
						 SMgrRelationData *smgr,
						 ForkNumber forknum,
						 BlockNumber blocknum,
						 int nblocks,
						 bool skip_fsync)
{
	PgAioTargetData *sd = pgaio_io_get_target_data(ioh);

	pgaio_io_set_target(ioh, PGAIO_TID_SMGR);

	/* backend is implied via IO owner */
	sd->smgr.rlocator = smgr->smgr_rlocator.locator;
	sd->smgr.forkNum = forknum;
	sd->smgr.blockNum = blocknum;
	sd->smgr.nblocks = nblocks;
	sd->smgr.is_temp = SmgrIsTemp(smgr);
	/* Temp relations should never be fsync'd */
	sd->smgr.skip_fsync = skip_fsync && !SmgrIsTemp(smgr);
}

/*
 * Callback for the smgr AIO target, to reopen the file (e.g. because the IO
 * is executed in a worker).
 */
static void
smgr_aio_reopen(PgAioHandle *ioh)
{
	PgAioTargetData *sd = pgaio_io_get_target_data(ioh);
	PgAioOpData *od = pgaio_io_get_op_data(ioh);
	SMgrRelation reln;
	ProcNumber	procno;
	uint32		off;

	/*
	 * The caller needs to prevent interrupts from being processed, otherwise
	 * the FD could be closed again before we get to executing the IO.
	 */
	Assert(!INTERRUPTS_CAN_BE_PROCESSED());

	if (sd->smgr.is_temp)
		procno = pgaio_io_get_owner(ioh);
	else
		procno = INVALID_PROC_NUMBER;

	reln = smgropen(sd->smgr.rlocator, procno);
	switch (pgaio_io_get_op(ioh))
	{
		case PGAIO_OP_INVALID:
			pg_unreachable();
			break;
		case PGAIO_OP_READV:
			od->read.fd = smgrfd(reln, sd->smgr.forkNum, sd->smgr.blockNum, &off);
			Assert(off == od->read.offset);
			break;
		case PGAIO_OP_WRITEV:
			od->write.fd = smgrfd(reln, sd->smgr.forkNum, sd->smgr.blockNum, &off);
			Assert(off == od->write.offset);
			break;
	}
}

/*
 * Callback for the smgr AIO target, describing the target of the IO.
 */
static char *
smgr_aio_describe_identity(const PgAioTargetData *sd)
{
	RelPathStr	path;
	char	   *desc;

	path = relpathbackend(sd->smgr.rlocator,
						  sd->smgr.is_temp ?
						  MyProcNumber : INVALID_PROC_NUMBER,
						  sd->smgr.forkNum);

	if (sd->smgr.nblocks == 0)
		desc = psprintf(_("file \"%s\""), path.str);
	else if (sd->smgr.nblocks == 1)
		desc = psprintf(_("block %u in file \"%s\""),
						sd->smgr.blockNum,
						path.str);
	else
		desc = psprintf(_("blocks %u..%u in file \"%s\""),
						sd->smgr.blockNum,
						sd->smgr.blockNum + sd->smgr.nblocks - 1,
						path.str);

	return desc;
}
