/*-------------------------------------------------------------------------
 *
 * vc_ops.c
 *	  Version control's operations on a branch, as Dolt's are.
 *
 * A commit is of its branch's working set as of one point, the barrier's
 * (prolly/capture.h): what capture has of its catalog branch there.  The
 * branch's head is locked before the barrier and to the end of the
 * transaction (prolly/branch_link.h), so no other commit of it writes the
 * head between, and its branch of version control is locked as the commit
 * is written, so the head the commit follows is the head then.  A barrier
 * waits for writers in progress, which a transaction that has written would
 * be among: it runs in one that hasn't.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/vc_ops.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "prolly/branch_link.h"
#include "prolly/capture.h"
#include "prolly/chunkstore.h"
#include "prolly/engine/vc_refs.h"
#include "prolly/vc_ops.h"
#include "utils/snapmgr.h"

/* Refused with Dolt's words: a name or email given as "". */
static void
ident_check(const VcIdent *id, const char *who)
{
	if (id->name && id->name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Aborting commit due to empty %s name. Is your config set?", who)));
	if (id->email && id->email[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Aborting commit due to empty %s email. Is your config set?", who)));
}

static void
options_check(const VcCommitOptions *opts)
{
	if (opts->allow_empty && opts->skip_empty)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("error: cannot use both --allow-empty and --skip-empty")));
	if (!opts->amend && opts->msg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Must provide commit message.")));
	if (!opts->amend && opts->msg[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Aborting commit due to empty commit message.")));
	ident_check(&opts->meta.author, "author");
	ident_check(&opts->meta.committer, "committer");
}

/* The commit, in a transaction that has written nothing; as commit. */
static bool
commit_made(const char *name, const VcCommitOptions *opts, int timeout_ms, PHash *out)
{
	ChunkStore *cs = ChunkStoreOpen("pages");
	int32		branch;
	ProllyCtx	ctx;
	VcRepo		repo;
	PHash		root;
	const char *msg = opts->msg && opts->msg[0] != '\0' ? opts->msg : NULL;
	int			flags = (opts->allow_empty ? VC_COMMIT_ALLOW_EMPTY : 0) |
		(opts->amend ? VC_COMMIT_AMEND : 0) | (opts->skip_empty ? VC_COMMIT_SKIP_EMPTY : 0);
	int			rc;

	/*
	 * (before the head's lock, as what it's to wait on can be waiting on what this has written; a
	 * commit writes too, so a transaction block holds one at most)
	 */
	if (TransactionIdIsValid(GetTopTransactionIdIfAny()))
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("a commit can't be made in a transaction that has written"),
				 errdetail("A commit writes, so a transaction block can make only one."),
				 errhint("Commit the transaction first, or make the commit with CALL outside a transaction block.")));
	prolly_ctx_init(&ctx, cs, &prolly_default_policy);
	branch = BranchLink->branch_of(name);
	if (branch != 0)
	{
		LockBranchHead(branch);
		/*
		 * (renamed, deleted or given back as it waited: it would commit what's no longer the
		 * branch's)
		 */
		if (BranchLink->branch_of(name) != branch)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("branch \"%s\" changed as it was being committed", name),
					 errhint("Try again.")));
		CaptureBranchBarrier(&ctx, branch, timeout_ms, &root);
	}
	vc_repo_open(&repo, cs, VcRefsOpen("pointers", cs), &prolly_default_policy);
	if (vc_checkout(&repo, name) != VC_OK)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" not found", name)));
	rc = vc_commit_with(&repo, branch != 0 ? &root : NULL, msg, &opts->meta, flags, out);
	vc_repo_free(&repo);
	prolly_ctx_free(&ctx);
	if (rc == VC_ERR_MERGING)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("you are in the middle of a merge -- cannot amend")));
	if (rc == VC_ERR_NOTHING && opts->skip_empty)
		return false;
	if (rc == VC_ERR_NOTHING)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("nothing to commit")));
	if (rc != VC_OK)
		elog(ERROR, "commit of branch \"%s\" failed: %d", name, rc);
	return true;
}

static bool
shared_commit(const char *branch, const VcCommitOptions *opts, bool nonatomic, int timeout_ms,
			  PHash *out)
{
	bool		made;

	options_check(opts);
	if (!nonatomic)
		return commit_made(branch, opts, timeout_ms, out);

	/* (the CALL's portal holds what's asked of this through each commit) */
	if (SPI_connect_ext(SPI_OPT_NONATOMIC) != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect_ext failed");
	SPI_commit();
	PushActiveSnapshot(GetTransactionSnapshot());
	made = commit_made(branch, opts, timeout_ms, out);
	PopActiveSnapshot();
	SPI_commit();
	SPI_finish();
	return made;
}

const VcOpsRoutine SharedVcOps = {
	.name = "shared",
	.commit = shared_commit,
};

const VcOpsRoutine *VcOps = &SharedVcOps;
