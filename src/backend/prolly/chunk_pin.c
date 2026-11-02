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
#include "utils/rel.h"

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

/* a pin's key: the store's index, and the chunk's name */
typedef struct PinKey
{
	Oid			idx;
	uint8		hash[CHUNK_HASH_LEN];
} PinKey;

static void
pin_key(PinKey *key, Relation idx, const uint8 *hash)
{
	memset(key, 0, sizeof(*key));
	key->idx = RelationGetRelid(idx);
	memcpy(key->hash, hash, CHUNK_HASH_LEN);
}

void
ChunkPin(Relation idx, const uint8 *hash)
{
	PinKey		key;

	if (!callback_registered)
	{
		RegisterXactCallback(forget_pins, NULL);
		callback_registered = true;
	}
	if (pins == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(PinKey);
		ctl.entrysize = sizeof(PinKey);
		ctl.hcxt = TopTransactionContext;
		pins = hash_create("chunk pins", 64, &ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	pin_key(&key, idx, hash);
	(void) hash_search(pins, &key, HASH_ENTER, NULL);
}

bool
ChunkPinned(Relation idx, const uint8 *hash)
{
	PinKey		key;

	if (pins == NULL)
		return false;
	pin_key(&key, idx, hash);
	return hash_search(pins, &key, HASH_FIND, NULL) != NULL;
}
