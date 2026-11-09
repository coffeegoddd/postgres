/*-------------------------------------------------------------------------
 *
 * vc_ops.h
 *	  Version control's operations on a branch of the database's
 *	  repository, as Dolt's are.
 *
 * Each is a sequence of steps over a branch's working set (its catalog
 * branch's rows and tables, prolly/branch_ops.h), capture
 * (prolly/capture.h) and its branch of version control
 * (prolly/engine/vc.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/vc_ops.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_VC_OPS_H
#define PROLLY_VC_OPS_H

#include "prolly/engine/vc.h"

/* A commit's options, Dolt's. */
typedef struct VcCommitOptions
{
	const char *msg;			/* NULL or "": only amending, the head's */
	VcCommitMeta meta;			/* as prolly/engine/vc.h has it */
	bool		allow_empty;	/* --allow-empty */
	bool		skip_empty;		/* --skip-empty */
	bool		amend;			/* --amend */
} VcCommitOptions;

typedef struct VcOpsRoutine
{
	const char *name;

	/*
	 * A commit of a branch's working set, all of it, as of one point: what
	 * capture has of it there, waited for for up to timeout_ms; a branch with
	 * no working set has its head as one.  The commit in *out, and true.
	 * With nothing to commit (its working set its head's, and no merge in
	 * progress), refused, or with skip_empty, false and nothing made; with
	 * allow_empty or amend, made all the same.  Amending, the commit takes
	 * the head's place, its parents, and its message if given none; refused
	 * while a merge is in progress.  Dolt's commit commits the SQL
	 * transaction first: so does this with nonatomic (a CALL outside a
	 * transaction block, whose transaction can be committed), the commit
	 * then made in a transaction of its own and that committed too.
	 * Unlike Dolt, in a transaction block, where Postgres lets nothing
	 * commit: it's made in the caller's transaction, which an abort then
	 * undoes, and refused once that has written, as what the commit waits
	 * for would wait on it.
	 */
	bool		(*commit) (const char *branch, const VcCommitOptions *opts, bool nonatomic,
						   int timeout_ms, PHash *out);
} VcOpsRoutine;

extern PGDLLIMPORT const VcOpsRoutine SharedVcOps;

/* The implementation in use. */
extern PGDLLIMPORT const VcOpsRoutine *VcOps;

#endif							/* PROLLY_VC_OPS_H */
