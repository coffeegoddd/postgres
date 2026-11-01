/*-------------------------------------------------------------------------
 *
 * chunk_pin.c
 *	  This transaction's holds on the history store's chunks
 *	  (prolly/chunk_pin.h).
 *
 * A hash table of the hashes pinned, in the transaction's memory, gone
 * with it.  A subtransaction aborted leaves its pins: the chunk is then
 * only passed by by this transaction's freeing, which it may do without.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_pin.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_pin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

static HTAB *pins = NULL;
static bool callback_registered = false;

static void
forget_pins(XactEvent event, void *arg)
{
	/* (the table is in the transaction's memory, gone with it) */
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
		event == XACT_EVENT_PREPARE || event == XACT_EVENT_PARALLEL_COMMIT ||
		event == XACT_EVENT_PARALLEL_ABORT)
		pins = NULL;
}

void
ChunkPin(const uint8 *hash)
{
	if (!callback_registered)
	{
		RegisterXactCallback(forget_pins, NULL);
		callback_registered = true;
	}
	if (pins == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = CHUNK_HASH_LEN;
		ctl.entrysize = CHUNK_HASH_LEN;
		ctl.hcxt = TopTransactionContext;
		pins = hash_create("chunk pins", 64, &ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	(void) hash_search(pins, hash, HASH_ENTER, NULL);
}

bool
ChunkPinned(const uint8 *hash)
{
	return pins != NULL && hash_search(pins, hash, HASH_FIND, NULL) != NULL;
}
