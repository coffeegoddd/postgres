/*-------------------------------------------------------------------------
 *
 * prolly_memo.h
 *	  Value memo interface: what a value TOAST stored out of line folds
 *	  to, found by its TOAST pointer.
 *
 * A fold encodes a row whose long column is a TOAST pointer by reading the
 * value back, decoding it and hashing it into a value tree.  A TOAST
 * pointer that an UPDATE of another column kept names the same value it
 * did, so the fold before may have done that work already: the memo keeps
 * the reference it made, and a fold that finds the pointer again writes
 * the reference without reading the value.
 *
 * A value ID alone doesn't name content: after VACUUM removes a value its
 * ID may be given to new content, and two branches (which share a TOAST
 * relation's OID, each with its own storage) may each give one ID to
 * different content.  So an implementation keys values by more than the
 * pointer (the "shared" one: also the xmin of the value's first chunk).
 *
 * Contract:
 * - lookup() gives the reference remember() was given for the value a
 *   pointer names now, in a column of type typid, or nothing; never a
 *   reference to other content, nor one whose value tree values' store
 *   doesn't have.
 * - remember() notes a reference the current transaction made.  The
 *   transaction sees it at once; others only once it commits, after the
 *   chunks it wrote are everyone's.  An abort (of the transaction, or of
 *   the subtransaction that remembered it) forgets it: a reference to
 *   chunks no one can read is never given out.
 * - A memo may forget anything at any time: a miss is only slower.
 * - counters() gives the lookups that hit and missed since the last reset.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_memo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_MEMO_H
#define PROLLY_MEMO_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"

struct ProllyValueStore;

/* What a memo's lookups found. */
typedef struct ProllyMemoCounters
{
	uint64		hits;
	uint64		misses;
} ProllyMemoCounters;

typedef struct ProllyValueMemoRoutine
{
	ProllyImplInfo info;

	/* Shared memory: its size (0 for none), and its setup, once. */
	Size		(*shmem_size) (void);
	void		(*shmem_init) (void *area);

	/*
	 * The reference the value pointer (an on-disk TOAST pointer) names was
	 * remembered by, in a column of type typid: appended to ref.
	 */
	bool		(*lookup) (const struct ProllyValueStore *values,
						   const void *pointer, Oid typid, StringInfo ref);
	void		(*remember) (const void *pointer, Oid typid,
							 const ProllyValue *ref);

	void		(*counters) (ProllyMemoCounters *out, bool reset);
} ProllyValueMemoRoutine;

#define PROLLY_MEMO_CALLBACK_NAMES \
	"shmem_size", "shmem_init", "lookup", "remember", "counters"
#define PROLLY_MEMO_FIRST_CALLBACK shmem_size
#define PROLLY_MEMO_LAST_CALLBACK counters

static inline const ProllyValueMemoRoutine *
prolly_memo_lookup(const char *name)
{
	return (const ProllyValueMemoRoutine *)
		prolly_get_impl(PROLLY_IFACE_VALUE_MEMO, name);
}

#endif							/* PROLLY_MEMO_H */
