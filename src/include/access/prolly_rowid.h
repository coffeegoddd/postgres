/*-------------------------------------------------------------------------
 *
 * prolly_rowid.h
 *	  Row identity interface: TIDs <-> rows.
 *
 * Postgres passes rows around by ItemPointer (index scans, triggers, EPQ,
 * CURRENT OF, ctid).  Prolly rows are identified by key.  Implementations:
 * content-derived TIDs with a map (design change 3), or wide row ids (B7,
 * open question 2).
 *
 * Contract:
 * - One TID names one row version: the same (key, content) always gets the
 *   same TID, and different versions get different TIDs (collisions are
 *   resolved by the implementation).
 * - tid_resolve() finds every TID that tid_assign() returned and that
 *   tid_forget() has not.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_rowid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ROWID_H
#define PROLLY_ROWID_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"

typedef struct ProllyRowIdRoutine
{
	ProllyImplInfo info;

	void		(*tid_assign) (Relation rel, const ProllyKey *key,
							   const ProllyHash *content, ItemPointer tid);

	/* Appends the row's key to key_out; false if tid is unknown. */
	bool		(*tid_resolve) (Relation rel, ItemPointer tid,
								StringInfo key_out, ProllyHash *content);

	void		(*tid_forget) (Relation rel, ItemPointer tid);
} ProllyRowIdRoutine;

#define PROLLY_ROWID_CALLBACK_NAMES "tid_assign", "tid_resolve", "tid_forget"
#define PROLLY_ROWID_FIRST_CALLBACK tid_assign

static inline const ProllyRowIdRoutine *
prolly_rowid_lookup(const char *name)
{
	return (const ProllyRowIdRoutine *)
		prolly_get_impl(PROLLY_IFACE_ROW_IDENTITY, name);
}

#endif							/* PROLLY_ROWID_H */
