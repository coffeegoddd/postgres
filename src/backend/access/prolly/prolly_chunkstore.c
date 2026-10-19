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
#include "access/prolly_config.h"
#include "access/prolly_hash.h"
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

bool
prolly_chunkstore_commit_writes(void)
{
	const ProllyChunkStoreRoutine *store = prolly_chunk_store();

	return PROLLY_CALL(store, commit_writes);
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

const ProllyStoreFormat *
prolly_store_format(void)
{
	static ProllyStoreFormat format;
	static bool known = false;

	if (!known)
	{
		const ProllyHashRoutine *hash = prolly_content_hash();
		const ProllyChunkFormatRoutine *chunks =
			prolly_chunkformat_lookup(CHUNK_FORMAT_IMPL);
		ProllyHashCtx *ctx;
		ProllyHash	empty;

		if (strlen(hash->info.name) >= PROLLY_STORE_HASH_NAME_LEN)
			elog(ERROR, "content hash name \"%s\" is too long", hash->info.name);
		/* (the digest's length is the same for every input) */
		ctx = PROLLY_CALL(hash, begin, CurrentMemoryContext);
		PROLLY_CALL(hash, finish, ctx, &empty);
		memset(&format, 0, sizeof(format));
		strlcpy(format.hash, hash->info.name, sizeof(format.hash));
		format.hash_len = empty.len;
		format.chunk_format = chunks->info.format_version;
		known = true;
	}
	return &format;
}

bool
prolly_store_format_equal(const ProllyStoreFormat *a, const ProllyStoreFormat *b)
{
	return strncmp(a->hash, b->hash, PROLLY_STORE_HASH_NAME_LEN) == 0 &&
		a->hash_len == b->hash_len &&
		a->chunk_format == b->chunk_format;
}

/* A format, for messages: "blake3 (20 bytes), chunk format 1". */
char *
prolly_store_format_text(const ProllyStoreFormat *format)
{
	return psprintf("%.*s (%u bytes), chunk format %u",
					PROLLY_STORE_HASH_NAME_LEN, format->hash,
					format->hash_len, format->chunk_format);
}
