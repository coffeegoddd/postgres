/*-------------------------------------------------------------------------
 *
 * prolly_window.h
 *	  History window interface: keeping a window of history locally, the
 *	  rest severed at a horizon, and repairing it again.
 *
 * Commits are named by their content, a commit's id covering its
 * parents', so history is never rewritten to shorten it.  It is severed
 * instead, as a shallow clone is: the oldest commits kept (the boundary)
 * still name their parents, and the horizon records that those parents
 * (the fragment's tops) and everything only they reach are kept elsewhere
 * (archived) or were let go of (deleted).  A collection then frees what
 * only the severed commits reached, following no boundary commit to the
 * parents the horizon names.  Repair brings a fragment's chunks back from
 * wherever they are (its archive, or any remote or clone that has them),
 * each checked against its hash and the whole against the fragment's
 * digest, and the history reads as it did.
 *
 * Contract:
 * - policy() gives the window branch name's lineage keeps (NULL: the
 *   database's), as the database's settings and the branch's own have it
 *   (never a session's); set_policy() sets a branch's own setting (value
 *   NULL: the database's again), the database owner's.
 * - plan() works out what a sever would cut (before not NULL: every commit
 *   dated before it, whatever the policy), without changing anything: the commits it would sever, the boundary edges,
 *   and the commits kept past the window (pinned).  A commit any ref
 *   names is kept; so is every commit on the way from a branch head to
 *   its merge base with each other branch head.  It holds every ref.
 * - sever() severs what plan() finds, as action says: archive (the
 *   fragment sent to the policy's archive, and found whole there, before
 *   the horizon records it; with no archive, nothing is severed, with a
 *   warning), delete (the horizon records the fragment, none of it kept
 *   elsewhere), or none (nothing).  It holds every ref and the horizon's lock, and is the
 *   database owner's.  Its result says what was done.  A collection after
 *   the transaction commits frees what only the fragment reached.
 * - maintain() severs the history window in the background, as sever()
 *   does with the database's action, as the database's owner, once a
 *   lineage's cut (rounded down to its batch; an hour where the window
 *   names none) has moved since the last it tried, or the size limit says
 *   so; a failure is reported, not raised, and retried after a wait that
 *   grows; true if it made a fragment.  collect_due() says whether the
 *   background's collection is to be a full one now (fragments made since
 *   the last, which ended long enough ago); collected() notes that one
 *   ran, and the store's size it left.
 * - archived() says whether commit id is past the horizon: a top of a
 *   fragment, named by a boundary commit.  edges() visits the horizon's
 *   boundary edges (a boundary commit and the parent it doesn't keep) as
 *   the refs at refs_root have them, each node read by read(read_arg),
 *   for a collection: it holds the horizon's lock shared until the
 *   transaction ends, and refuses a transaction that changed the horizon.
 *   fragments() visits the horizon's fragments, newest first.  hold()
 *   takes the horizon's lock shared until the transaction ends, for a
 *   collection, before it begins (the lock comes before the store's
 *   collection, as before the refs' for a sever or repair), refusing a
 *   transaction that changed the horizon.
 * - plan() reads the refs as they stand, holding none.
 * - window() visits the commits in the window now (branch not NULL: in
 *   that branch's lineage's; NULL: in any branch's), each once, until
 *   visit returns false: those its lineage's policy keeps for their age
 *   or count, by the clock a sever ages them by (no commit older than one
 *   beneath it; a commit dated past now aged from its arrival here), the
 *   cut taken once for the whole evaluation.  Not the commits kept only
 *   because a ref names them or for a merge base.  A lineage with no
 *   window keeps all its history here, so all of it is in the window.
 *   in_window() says whether commit is among them: a sever never severs a
 *   commit in_window() says is in the window.  Both read the refs as they
 *   stand, holding none.
 * - repair() reattaches the newest nfragments of them (< 0: all), newest
 *   first, from source (a remote's name or URL; NULL: each fragment's
 *   archive), and lets go of the horizon's record of each only once its
 *   chunks are back, each re-hashed as received, and its commits give its
 *   identity and digest again; otherwise nothing changes.  It holds every
 *   ref and the horizon's lock, and is the database owner's.  The count
 *   repaired.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_window.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WINDOW_H
#define PROLLY_WINDOW_H

#include "access/prolly_refs.h"
#include "access/prolly_registry.h"
#include "datatype/timestamp.h"

/* What is done with history past the window (history_window_action). */
typedef enum ProllyWindowAction
{
	PROLLY_WINDOW_ARCHIVE,		/* archived, verified, then freed */
	PROLLY_WINDOW_DELETE,		/* freed, not archived */
	PROLLY_WINDOW_NONE,			/* kept */
} ProllyWindowAction;

/*
 * A window: what a branch's lineage keeps here.  A commit is kept if it
 * is dated within keep_time of now or among the keep_commits newest of
 * its head (either, where both are set; neither: everything is kept).
 * batch, if set, rounds the cut down to a whole multiple of it, so
 * fragments are whole hours or days.  max_bytes is the most the history
 * kept here may hold (0: no limit).  action is what is done with history
 * past the window, and archive where it goes (a remote's name or URL;
 * NULL: nowhere, so archive mode severs nothing).
 */
typedef struct ProllyWindowPolicy
{
	int64		keep_commits;	/* 0: not set */
	int64		keep_time;		/* microseconds; 0: not set */
	int64		max_bytes;		/* 0: not set */
	int64		batch;			/* microseconds; 0: not set */
	ProllyWindowAction action;
	char	   *archive;
} ProllyWindowPolicy;

/* A commit a plan severs, or a boundary edge, or a pin. */
typedef enum ProllyWindowItemKind
{
	PROLLY_WINDOW_SEVERED,		/* a commit severed */
	PROLLY_WINDOW_EDGE,			/* commit is a boundary commit, other the
								 * parent it doesn't keep */
	PROLLY_WINDOW_PINNED,		/* a commit kept past its window: a tag's,
								 * a merge base's, a ref's */
} ProllyWindowItemKind;

typedef void (*ProllyWindowItemVisit) (ProllyWindowItemKind kind,
									   const ProllyHash *commit,
									   const ProllyHash *other,
									   const char *why, void *arg);

/* What a sever did. */
typedef struct ProllyWindowResult
{
	ProllyWindowAction action;
	int64		commits;		/* severed */
	int64		edges;			/* boundary edges made */
	int64		chunks;			/* in the fragment */
	int64		bytes;			/* of those */
	int64		sent;			/* chunks sent to the archive */
	int64		fragment;		/* its number; 0: none made */
	int			nboundary;
	ProllyHash *boundary;		/* the boundary commits (palloc'd) */
} ProllyWindowResult;

/* A fragment of the horizon. */
typedef struct ProllyWindowFragment
{
	int64		number;
	bool		deleted;		/* let go of, not archived */
	TimestampTz severed_at;
	TimestampTz oldest;			/* its commits' dates */
	TimestampTz newest;
	int64		commits;
	int64		chunks;
	int64		bytes;
	ProllyHash	root;			/* its identity: its commits' index tree */
	ProllyHash	digest;			/* over its chunks' hashes, sorted */
	const char *location;		/* its archive's URL, or NULL */
	int			ntops;
	const ProllyHash *tops;
} ProllyWindowFragment;

/* A commit in the window; false stops the visit. */
typedef bool (*ProllyWindowCommitVisit) (const ProllyHash *commit, void *arg);

typedef void (*ProllyWindowFragmentVisit) (const ProllyWindowFragment *f,
										   void *arg);
typedef void (*ProllyWindowEdgeVisit) (const ProllyHash *boundary,
									   const ProllyHash *parent, void *arg);

typedef struct ProllyWindowRoutine
{
	ProllyImplInfo info;

	void		(*plan) (const TimestampTz *before, ProllyWindowItemVisit visit,
						 void *arg);
	void		(*sever) (ProllyWindowAction action, const TimestampTz *before,
						  ProllyWindowResult *result);
	bool		(*archived) (const ProllyHash *id);
	void		(*edges) (const ProllyHash *refs_root, ProllyChunkRead read,
						  void *read_arg, ProllyWindowEdgeVisit visit,
						  void *arg);
	void		(*fragments) (ProllyWindowFragmentVisit visit, void *arg);
	void		(*hold) (void);
	int			(*repair) (const char *source, int nfragments);
	void		(*policy) (const char *name, ProllyWindowPolicy *policy);
	void		(*set_policy) (const char *name, const char *setting,
							   const char *value);
	bool		(*maintain) (void);
	bool		(*collect_due) (void);
	void		(*collected) (void);
	void		(*window) (const char *branch, ProllyWindowCommitVisit visit,
						   void *arg);
	bool		(*in_window) (const char *branch, const ProllyHash *commit);
} ProllyWindowRoutine;

#define PROLLY_WINDOW_CALLBACK_NAMES \
	"plan", "sever", "archived", "edges", "fragments", "hold", "repair", \
	"policy", "set_policy", "maintain", "collect_due", "collected", \
	"window", "in_window"
#define PROLLY_WINDOW_FIRST_CALLBACK plan
#define PROLLY_WINDOW_LAST_CALLBACK in_window

static inline const ProllyWindowRoutine *
prolly_window_lookup(const char *name)
{
	return (const ProllyWindowRoutine *)
		prolly_get_impl(PROLLY_IFACE_HISTORY_WINDOW, name);
}

/*
 * An error if commit id is past the history horizon (archived or
 * deleted), saying how to repair it; nothing otherwise.  For readers that
 * found a commit missing from the store.
 */
extern void prolly_window_check_missing(const ProllyHash *id);

/* Defines the prolly.history_* settings, once a backend. */
extern void prolly_window_init(void);

/*
 * As prolly_window_check_missing(), and if the horizon holds any fragment,
 * an error that commit id, named but not in the store, may be past it:
 * for a commit named by its id, which the horizon's tops don't cover when
 * it is beneath them.
 */
extern void prolly_window_check_absent(const ProllyHash *id);

/* Is commit id past the history horizon (its data severed)? */
extern bool prolly_window_archived(const ProllyHash *id);

/*
 * For a reader that found a tree missing from the store: an error, if it
 * is a severed commit's root or dependencies' tree, that the commit is
 * past the history horizon, saying how to repair it; nothing otherwise.
 */
extern void prolly_window_check_root(const ProllyHash *tree);

/*
 * For readers of every commit's rows (a history table, history mounts):
 * is commit id's data severed, so it has none to read?  A notice says so,
 * once a transaction.  Its commit, and the history past it, are listed
 * still.
 */
extern bool prolly_window_rows_severed(const ProllyHash *id);

#endif							/* PROLLY_WINDOW_H */
