/*-------------------------------------------------------------------------
 *
 * prolly_writers.h
 *	  Writers interface: who may change a branch's working set, and when.
 *
 * A branch's working set is every change made to it since its head commit.
 * In the shared mode (the default) every session on a branch writes into
 * it, as in Dolt, and a commit records all of it.  In the stricter modes a
 * dirty branch has one owner, the writer (dolt.writer: by default the
 * session) whose changes it holds, so a commit records one writer's work:
 * a clean branch is claimed by the first write to it, and a dirty one
 * refuses every other writer until it is clean again (committed, reset,
 * or taken over).  A protected branch has no writers at all: it changes
 * only by operations that make their commit in the same transaction
 * (integrations: a merge, cherry-pick, revert, reset or commit), so its
 * working set is its head at every snapshot.  In the branch-only mode
 * main is protected unless unprotected; any branch can be protected in
 * any mode.
 *
 * Contract:
 * - note_write() is called before the transaction first changes a branch
 *   (catalog/branch.h's branch_note_write(), and again after a subtransaction
 *   that first changed it aborts); it claims the branch for the writer or
 *   refuses the write with an error.  A claim is the transaction's: it is
 *   undone if the transaction aborts.  An integration is allowed on a
 *   protected branch, holding it exclusively to transaction end; its
 *   caller makes sure it commits.
 * - settled() is called once the session's branch may be clean again (its
 *   working set committed or reset); it forgets the owner if the branch is
 *   clean and no other backend of the writer is writing it, and then the
 *   transaction's next write to it claims it again.
 * - take() makes a dirty branch the session's writer's, for the database
 *   owner or the role that holds it (a clean one has nothing to take).
 * - mode() and set_mode() read and set the database's mode, and
 *   protected() and set_protected() a branch's protection (by name: a
 *   branch with only a head has one too); setting either needs the
 *   database's ownership.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_writers.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WRITERS_H
#define PROLLY_WRITERS_H

#include "access/prolly_registry.h"

typedef enum ProllyWriterMode
{
	PROLLY_WRITERS_SHARED,		/* every session writes the branch */
	PROLLY_WRITERS_OWNED,		/* one writer per dirty branch */
	PROLLY_WRITERS_BRANCH_ONLY, /* owned, and main protected */
} ProllyWriterMode;

/* the modes' names, by ProllyWriterMode */
#define PROLLY_WRITER_MODE_NAMES \
	[PROLLY_WRITERS_SHARED] = "shared", \
	[PROLLY_WRITERS_OWNED] = "owned", \
	[PROLLY_WRITERS_BRANCH_ONLY] = "branch-only"

/* a branch's protection, as set */
typedef enum ProllyBranchProtection
{
	PROLLY_PROTECT_DEFAULT,		/* as the mode has it */
	PROLLY_PROTECT_ON,
	PROLLY_PROTECT_OFF,
} ProllyBranchProtection;

typedef struct ProllyWritersRoutine
{
	ProllyImplInfo info;

	/* the transaction is about to change branch: claim it, or refuse */
	void		(*note_write) (Oid branch, bool integration);

	/* branch may be clean again: forget its owner if it is */
	void		(*settled) (Oid branch);

	/* make dirty branch the session's writer's */
	void		(*take) (Oid branch);

	ProllyWriterMode (*mode) (void);
	void		(*set_mode) (ProllyWriterMode mode);
	bool		(*protected) (const char *name);
	void		(*set_protected) (const char *name,
								  ProllyBranchProtection protection);
} ProllyWritersRoutine;

#define PROLLY_WRITERS_CALLBACK_NAMES \
	"note_write", "settled", "take", "mode", "set_mode", "protected", \
	"set_protected"
#define PROLLY_WRITERS_FIRST_CALLBACK note_write
#define PROLLY_WRITERS_LAST_CALLBACK set_protected

/* the session's writer: dolt.writer, or one of the session's own */
extern char *prolly_current_writer(void);

/* implementation "modes": its settings, defined once per backend */
extern void prolly_writers_init(void);

#endif							/* PROLLY_WRITERS_H */
