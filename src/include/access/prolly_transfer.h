/*-------------------------------------------------------------------------
 *
 * prolly_transfer.h
 *	  Transfer interface: clone, fetch and push between databases.
 *
 * Transfer moves canonical chunks (engine export_chunks) and refs between
 * this database and a remote.  Implementations differ in transport: a local
 * copy between two instances, or a network protocol (open question 14).
 *
 * Contract:
 * - Only canonical chunks cross, so the remote may use any engine
 *   (specs/05-storage-targets.md).
 * - Chunks the other side already has are not sent.
 * - A failed transfer leaves refs unchanged on both sides.
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
#include "access/prolly_types.h"

typedef struct ProllyTransferRoutine
{
	ProllyImplInfo info;

	/* Copy every branch and commit of remote_url into this (empty) database. */
	void		(*clone) (const char *remote_url);

	/* Bring refname and the chunks it needs from remote_url. */
	void		(*fetch) (const char *remote_url, const char *refname);

	/* Send branch and the chunks it needs to remote_url. */
	void		(*push) (const char *remote_url, const char *branch);
} ProllyTransferRoutine;

#define PROLLY_TRANSFER_CALLBACK_NAMES "clone", "fetch", "push"
#define PROLLY_TRANSFER_FIRST_CALLBACK clone

static inline const ProllyTransferRoutine *
prolly_transfer_lookup(const char *name)
{
	return (const ProllyTransferRoutine *)
		prolly_get_impl(PROLLY_IFACE_TRANSFER, name);
}

#endif							/* PROLLY_TRANSFER_H */
