/*-------------------------------------------------------------------------
 *
 * prolly_rootretain.h
 *	  Root retention interface: which of a branch's kept commits keep their
 *	  page roots.
 *
 * Each commit a branch keeps its pages as of holds a root of each of its
 * relations' storage (the refs' kept commits), so that a move to it gives
 * the tables those pages back in O(1).  A move to a commit without one
 * gives back the nearest one's before it, and writes the rows the two
 * differ by.  Retention decides which commits keep theirs: the refs ask it
 * in retention passes, over a branch's kept commits from the oldest (by
 * height, the order history ages in along the branch), and retire those
 * it retires, until it keeps one.
 *
 * Contract:
 * - max_bytes() is the most the roots of every branch may hold (0: no
 *   limit), as the database has it.
 * - due() says whether a pass of branch's kept commits is due, nlive of
 *   them kept and keeps of them made since its last pass.
 * - begin() starts a pass of branch's, excess being how far the roots of
 *   every branch hold more than max_bytes() (<= 0: they don't), and gives
 *   its state; without windows (a pass for the cap alone, not due), every
 *   commit is taken as in the branch's window, so the pass only thins; end() ends it, saying how much of the excess is left (no
 *   more of it the branch's to retire).
 * - judge() is given each of the branch's kept commits, oldest first, and
 *   says to retire it (PROLLY_ROOT_RETIRE), to keep it and go on
 *   (PROLLY_ROOT_PASS), or to keep it and end the pass (PROLLY_ROOT_STOP);
 *   then those left, newest first (from_newest), the same way: a commit
 *   a branch was reset or amended away from is above what it has now.
 *   It never retires the newest root, nor a tagged commit's: a tag keeps
 *   its commit's root while it names it.  A commit in another lineage's
 *   window only is that lineage's to keep a root of: judged
 *   PROLLY_ROOT_RETIRE_IF_KEPT, the refs retire it only if another branch
 *   keeps one, holding that branch against its own passes (so two
 *   branches' passes can't each let go of theirs on the other's strength).  The work of a pass is in the
 *   commits it retires and passes over.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_rootretain.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ROOTRETAIN_H
#define PROLLY_ROOTRETAIN_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "datatype/timestamp.h"

/* A kept commit's root, as a retention pass is given it. */
typedef struct ProllyRetainedRoot
{
	ProllyHash	commit;
	int64		height;			/* the commit's (0: not known) */
	int64		bytes;			/* what its root holds that the one before
								 * it doesn't */
	TimestampTz kept_at;
	bool		newest;			/* the branch's newest: never retired */
	bool		tagged;			/* a tag names its commit: never retired */
	bool		from_newest;	/* judged in the pass from the newest */
} ProllyRetainedRoot;

typedef enum ProllyRootVerdict
{
	PROLLY_ROOT_STOP,			/* kept, and the pass ends */
	PROLLY_ROOT_PASS,			/* kept, and the pass goes on */
	PROLLY_ROOT_RETIRE,			/* retired */
	PROLLY_ROOT_RETIRE_IF_KEPT, /* retired if another branch keeps a root
								 * of it (and none of its keeping is in
								 * flight), else passed over */
} ProllyRootVerdict;

typedef struct ProllyRootRetentionRoutine
{
	ProllyImplInfo info;

	int64		(*max_bytes) (void);
	bool		(*due) (const char *branch, int64 nlive, int64 keeps);
	void	   *(*begin) (const char *branch, int64 excess, bool windows);
	ProllyRootVerdict (*judge) (void *pass, const ProllyRetainedRoot *root);
	int64		(*end) (void *pass);
} ProllyRootRetentionRoutine;

#define PROLLY_ROOTRETAIN_CALLBACK_NAMES \
	"max_bytes", "due", "begin", "judge", "end"
#define PROLLY_ROOTRETAIN_FIRST_CALLBACK max_bytes
#define PROLLY_ROOTRETAIN_LAST_CALLBACK end

#endif							/* PROLLY_ROOTRETAIN_H */
