/*-------------------------------------------------------------------------
 *
 * prolly_transfer.h
 *	  Transfer interface: moving commits between this database's chunk
 *	  store and a remote (prolly_remote.h).
 *
 * Contract:
 * - A store that has a chunk has every chunk it references: so what the
 *   other side has, and all below it, isn't sent, and what is received is
 *   checked to hold before the store takes it.
 * - What receive() and send() are given may be tag objects as well as
 *   commits (the refs' tag_get()): a tag's object moves, then its commit,
 *   as the refs' walk_commit() reaches them.
 * - Only chunks move: refs (this database's remote-tracking branches, the
 *   remote's heads) are the caller's to move afterwards.  A receive that
 *   fails adds nothing; a send that fails adds nothing the remote reads.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_transfer.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_TRANSFER_H
#define PROLLY_TRANSFER_H

#include "access/prolly_registry.h"
#include "access/prolly_remote.h"
#include "access/prolly_types.h"

typedef struct ProllyTransferRoutine
{
	ProllyImplInfo info;

	/*
	 * Receive from remote what the commits reach that this database's
	 * store lacks, and add it to the store, at once: every chunk re-hashed
	 * against its address, every reference resolved (in the store or what
	 * was received), every commit's root of this build's format, before
	 * any is added.  The count of chunks received.
	 */
	uint64		(*receive) (const ProllyRemoteRoutine *rr, ProllyRemote *remote,
							const ProllyHash *commits, int ncommits);

	/*
	 * Send remote what the commits reach that it lacks, at once.  The count
	 * of chunks sent.
	 */
	uint64		(*send) (const ProllyRemoteRoutine *rr, ProllyRemote *remote,
						 const ProllyHash *commits, int ncommits);

	/*
	 * Optional, for a database with nothing to walk against (a clone's):
	 * receive the remote's files whole, then check them as receive() checks
	 * what it receives, before any is added; false, having added nothing,
	 * if it can't (the remote can't list them, or its journal is a large
	 * part of it: receive() then).  *count is the chunks received.
	 */
	bool		(*clone) (const ProllyRemoteRoutine *rr, ProllyRemote *remote,
						  const ProllyHash *commits, int ncommits,
						  uint64 *count);
} ProllyTransferRoutine;

#define PROLLY_TRANSFER_CALLBACK_NAMES "receive", "send", "clone"
#define PROLLY_TRANSFER_FIRST_CALLBACK receive
#define PROLLY_TRANSFER_LAST_CALLBACK clone

#endif							/* PROLLY_TRANSFER_H */
