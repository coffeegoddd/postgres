/*-------------------------------------------------------------------------
 *
 * vc_refs.h
 *	  The rows a repository's state is kept in: named references, each a
 *	  chunk's hash, each holding that chunk while it names it.
 *
 * Version control keeps its state in references, not in one chunk: a
 * branch is one (its head, working root and merge state, in a branch
 * chunk), and every commit the window's history keeps is one, holding
 * itself.  A commit sets its own reference and moves its branch's, so
 * commits on different branches change different rows and never wait on
 * each other.
 *
 * A reference holds the chunk it names as a root holds it (prolly/
 * chunkstore.h): set takes a root on the new chunk, lets go of the old
 * one's, and remove lets go.  A reference to a chunk the store hasn't is
 * an error.  Whether a reference's change is its transaction's is the
 * implementation's: "pointers" (rows of pg_history.pointers, whose trigger
 * keeps their roots) changes them in the user's transaction, gone if it
 * aborts; "memory" (for unit tests, on the memory store) at once.
 *
 * get with for_update also locks the reference until the transaction ends,
 * for a change that reads it first (a commit reads its branch's head): a
 * second such get waits for the first transaction to end, and then reads
 * what it left.  Locking a reference not there locks nothing.
 *
 * Implementations register with the registry (prolly/registry.h) under
 * VC_REFS_IFACE.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/engine/vc_refs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ENGINE_VC_REFS_H
#define PROLLY_ENGINE_VC_REFS_H

#include "prolly/chunkstore.h"
#include "prolly/engine/phash.h"

#define VC_REFS_IFACE	"vc_refs"

typedef struct VcRefs VcRefs;

/* A reference as a listing sees it. */
typedef void (*VcRefFn) (void *arg, const char *name, const PHash *hash);

typedef struct VcRefsRoutine
{
	/* The references, on the store their chunks are in. */
	VcRefs	   *(*open) (ChunkStore *cs);

	/* The chunk name names, in *out; false if there is no such reference. */
	bool		(*get) (VcRefs *refs, const char *name, bool for_update,
						PHash *out);

	/* name made, or moved, to name hash. */
	void		(*set) (VcRefs *refs, const char *name, const PHash *hash);

	/*
	 * name made, naming hash, unless there is one already (false): of two
	 * making one name at once, one makes it and the other finds it.
	 */
	bool		(*add) (VcRefs *refs, const char *name, const PHash *hash);

	/* name removed; false if there was none. */
	bool		(*remove) (VcRefs *refs, const char *name);

	/* fn called with each reference whose name starts with prefix, in order. */
	void		(*list) (VcRefs *refs, const char *prefix, VcRefFn fn, void *arg);
} VcRefsRoutine;

struct VcRefs
{
	const VcRefsRoutine *routine;
	ChunkStore *cs;
};

extern PGDLLIMPORT const VcRefsRoutine PointersVcRefs;
extern PGDLLIMPORT const VcRefsRoutine MemoryVcRefs;

/* The references registered as name, on cs; an error if there are none. */
extern VcRefs *VcRefsOpen(const char *name, ChunkStore *cs);

/*
 * For tests: forgets every reference the memory implementation has (with
 * the memory store emptied, whose roots they were).
 */
extern void MemoryVcRefsReset(void);

static inline bool
vc_refs_get(VcRefs *refs, const char *name, bool for_update, PHash *out)
{
	return refs->routine->get(refs, name, for_update, out);
}

static inline void
vc_refs_set(VcRefs *refs, const char *name, const PHash *hash)
{
	refs->routine->set(refs, name, hash);
}

static inline bool
vc_refs_add(VcRefs *refs, const char *name, const PHash *hash)
{
	return refs->routine->add(refs, name, hash);
}

static inline bool
vc_refs_remove(VcRefs *refs, const char *name)
{
	return refs->routine->remove(refs, name);
}

static inline void
vc_refs_list(VcRefs *refs, const char *prefix, VcRefFn fn, void *arg)
{
	refs->routine->list(refs, prefix, fn, arg);
}

#endif							/* PROLLY_ENGINE_VC_REFS_H */
