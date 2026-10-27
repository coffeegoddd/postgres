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
 * - plan() works out what a sever before date before would cut, without
 *   changing anything: the commits it would sever, the boundary edges,
 *   and the commits kept past the window (pinned).  A commit any ref
 *   names is kept; so is every commit on the way from a branch head to
 *   its merge base with each other branch head.  It holds every ref.
 * - sever() severs what plan() finds, as action says: delete (the
 *   horizon records the fragment, none of it kept elsewhere), or none
 *   (nothing).  It holds every ref and the horizon's lock, and is the
 *   database owner's.  Its result says what was done.  A collection after
 *   the transaction commits frees what only the fragment reached.
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
} ProllyWindowRoutine;

#define PROLLY_WINDOW_CALLBACK_NAMES \
	"plan", "sever", "archived", "edges", "fragments", "hold", "repair"
#define PROLLY_WINDOW_FIRST_CALLBACK plan
#define PROLLY_WINDOW_LAST_CALLBACK repair

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

/*
 * As prolly_window_check_missing(), and if the horizon holds any fragment,
 * an error that commit id, named but not in the store, may be past it:
 * for a commit named by its id, which the horizon's tops don't cover when
 * it is beneath them.
 */
extern void prolly_window_check_absent(const ProllyHash *id);

/*
 * Is any history severed (does the horizon hold a fragment)?  Readers that
 * take a commit's recorded counts of its history for its listing's ask.
 */
extern bool prolly_window_severed(void);

/* Is commit id past the history horizon? */
extern bool prolly_window_archived(const ProllyHash *id);

/*
 * For readers that list history (a log, a history table): is parent past
 * the horizon, so the listing stops there?  A notice says so, once a
 * transaction.
 */
extern bool prolly_window_stops_at(const ProllyHash *parent);

#endif							/* PROLLY_WINDOW_H */
