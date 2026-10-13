/*-------------------------------------------------------------------------
 *
 * prolly_log.h
 *	  Log interface: what prolly WAL records contain and how they replay.
 *
 * Implementations differ in WAL form (specs/03-design-space.md, axis 2):
 * full chunks, deltas, or logical row changes.  The engine reports events;
 * the log implementation decides what, if anything, each event writes.  A
 * resource-manager adapter (A3) dispatches redo/desc/identify/mask here.
 *
 * Contract:
 * - After crash recovery, every tree is as of its last committed root.
 * - log_node() is called with the buffer exclusive-locked and inside the
 *   caller's critical section, as for any Postgres WAL-logged page change.
 * - When wal_level = logical, enough row-level information is written for
 *   logical decoding, whatever the WAL form.
 *
 * Provisional (v0).  desc and identify must also run inside pg_waldump,
 * which has no registry; how frontend code reaches them is open.
 *
 * src/include/access/prolly_log.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_LOG_H
#define PROLLY_LOG_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "access/xlogdefs.h"
#include "lib/stringinfo.h"
#include "storage/buf.h"
#include "utils/relcache.h"

struct XLogReaderState;

typedef struct ProllyLogRoutine
{
	ProllyImplInfo info;

	/* A node was written (is_new) or changed in place. */
	void		(*log_node) (Relation tree, Buffer buf, bool is_new);

	/* A row was written (value) or deleted (value == NULL) in tree. */
	void		(*log_mutation) (Relation tree, const ProllyKey *key,
								 const ProllyValue *value);

	/* The transaction's new root for branch; returns the record's end. */
	XLogRecPtr	(*log_commit) (ProllyBranchId branch,
							   const ProllyNodeRef *new_root);

	/* Resource-manager callbacks (RmgrData). */
	void		(*redo) (struct XLogReaderState *record);
	void		(*desc) (StringInfo buf, struct XLogReaderState *record);
	const char *(*identify) (uint8 info);
	void		(*startup) (void);
	void		(*cleanup) (void);
	void		(*mask) (char *pagedata, BlockNumber blkno);
} ProllyLogRoutine;

#define PROLLY_LOG_CALLBACK_NAMES \
	"log_node", "log_mutation", "log_commit", "redo", "desc", "identify", \
	"startup", "cleanup", "mask"
#define PROLLY_LOG_FIRST_CALLBACK log_node

static inline const ProllyLogRoutine *
prolly_log_lookup(const char *name)
{
	return (const ProllyLogRoutine *)
		prolly_get_impl(PROLLY_IFACE_LOG, name);
}

#endif							/* PROLLY_LOG_H */
