/*-------------------------------------------------------------------------
 *
 * prolly_chunkstore.c
 *	  The server's chunk store, and the hooks commit, checkpoints and redo
 *	  call through it.
 *
 * A root move's commit-record payload, and every ProllyChunks record,
 * starts with the impl_id of the implementation that wrote it, so redo
 * reaches that implementation through the registry.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_chunkstore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_chunkstore.h"
#include "access/prolly_tree.h"
#include "access/prolly_xlog.h"

/* the implementation the server's stores use */
#define CHUNK_STORE_IMPL	"journal"

/* the trees, and the node format, of the chunks they keep */
#define CHUNK_TREE_IMPL		"cow"
#define CHUNK_FORMAT_IMPL	"basic"

const ProllyChunkStoreRoutine *
prolly_chunk_store(void)
{
	static const ProllyChunkStoreRoutine *routine = NULL;

	if (routine == NULL)
		routine = prolly_chunkstore_lookup(CHUNK_STORE_IMPL);
	return routine;
}

static const ProllyChunkStoreRoutine *
store_by_id(uint16 impl_id)
{
	const ProllyImplInfo *impl = prolly_lookup_impl_id(PROLLY_IFACE_CHUNK_STORE,
													   impl_id);

	if (impl == NULL)
		elog(PANIC, "unknown chunk store implementation %u", impl_id);
	return (const ProllyChunkStoreRoutine *) impl;
}

/*
 * What the commit record carries for the transaction's root move, if any:
 * the implementation's id, then its payload.
 */
Size
prolly_chunkstore_commit_payload(char **data)
{
	const ProllyChunkStoreRoutine *store = prolly_chunk_store();
	char	   *impl_data;
	Size		len = PROLLY_CALL(store, commit_payload, &impl_data);
	char	   *buf;

	if (len == 0)
		return 0;
	buf = palloc(sizeof(uint16) + len);
	memcpy(buf, &store->info.impl_id, sizeof(uint16));
	memcpy(buf + sizeof(uint16), impl_data, len);
	*data = buf;
	return sizeof(uint16) + len;
}

void
prolly_chunkstore_committed(void)
{
	const ProllyChunkStoreRoutine *store = prolly_chunk_store();

	PROLLY_CALL(store, committed);
}

void
prolly_chunkstore_commit_redo(const char *data, Size len)
{
	uint16		impl_id;
	const ProllyChunkStoreRoutine *store;

	if (len < sizeof(uint16))
		elog(PANIC, "chunk store commit payload too short");
	memcpy(&impl_id, data, sizeof(uint16));
	store = store_by_id(impl_id);
	PROLLY_CALL(store, commit_redo, data + sizeof(uint16), len - sizeof(uint16));
}

void
prolly_chunkstore_checkpoint(void)
{
	const ProllyChunkStoreRoutine *store = prolly_chunk_store();

	if (PROLLY_HAS(store, checkpoint))
		store->checkpoint();
}

void
prolly_chunks_redo(XLogReaderState *record)
{
	xl_prolly_header hdr;
	const ProllyChunkStoreRoutine *store;

	if (XLogRecGetDataLen(record) < SizeOfProllyHeader)
		elog(PANIC, "ProllyChunks record without a header");
	memcpy(&hdr, XLogRecGetData(record), SizeOfProllyHeader);
	store = store_by_id(hdr.impl_id);
	PROLLY_CALL(store, redo, record);
}

void
prolly_chunk_references(const void *data, Size len, ProllyChunkRefVisit visit,
						void *arg)
{
	static const ProllyTreeRoutine *tree = NULL;
	static const ProllyChunkFormatRoutine *format = NULL;

	if (tree == NULL)
	{
		format = prolly_chunkformat_lookup(CHUNK_FORMAT_IMPL);
		tree = prolly_tree_lookup(CHUNK_TREE_IMPL);
	}
	PROLLY_CALL(tree, references, format, data, len, visit, arg);
}
