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
#include "access/prolly_tablefile.h"
#include "access/prolly_config.h"
#include "access/prolly_hash.h"
#include "access/prolly_tree.h"
#include "access/prolly_xlog.h"
#include "storage/sync.h"
#include "utils/memutils.h"

#ifdef USE_LZ4
#include <lz4.h>
#endif

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

/*
 * A sync request's file, fsynced by the implementation that made the
 * request (its impl_id is the tag's forknum).
 */
int
prolly_chunks_syncfiletag(const FileTag *ftag, char *path)
{
	const ProllyChunkStoreRoutine *store = store_by_id((uint16) ftag->forknum);

	if (!PROLLY_HAS(store, sync_file))
		elog(ERROR, "chunk store \"%s\" makes no sync requests",
			 store->info.name);
	return store->sync_file(ftag, path);
}

/* (a database's requests, as a filter request names them) */
bool
prolly_chunks_filetagmatches(const FileTag *ftag, const FileTag *candidate)
{
	return ftag->rlocator.dbOid == candidate->rlocator.dbOid;
}

void
prolly_chunkstore_consistent(void)
{
	const ProllyChunkStoreRoutine *store = prolly_chunk_store();

	if (PROLLY_HAS(store, redo_consistent))
		store->redo_consistent();
}

void
prolly_chunkstore_recovered(void)
{
	const ProllyChunkStoreRoutine *store = prolly_chunk_store();

	if (PROLLY_HAS(store, recovered))
		store->recovered();
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

/* (a session's: a remote serving it sends its chunks uncompressed) */
bool		prolly_serve_uncompressed = false;

/* A frame of the chunk as it is, uncompressed. */
static void
frame_plain(const void *data, Size len, StringInfo out)
{
	char		head[PROLLY_FRAME_HEADER];

	if (len > PG_UINT32_MAX)
		elog(ERROR, "chunk of %zu bytes is too large to frame", len);
	head[0] = PROLLY_CODEC_NONE;
	head[1] = len & 0xff;
	head[2] = (len >> 8) & 0xff;
	head[3] = (len >> 16) & 0xff;
	head[4] = (len >> 24) & 0xff;
	appendBinaryStringInfo(out, head, PROLLY_FRAME_HEADER);
	appendBinaryStringInfo(out, data, len);
}

void
prolly_chunk_frame_plain(const char *frame, Size len, StringInfo out)
{
	StringInfoData chunk;

	if (len >= 1 && (uint8) frame[0] == PROLLY_CODEC_NONE)
	{
		appendBinaryStringInfo(out, frame, len);
		return;
	}
	initStringInfo(&chunk);
	if (!prolly_chunk_unframe(frame, len, &chunk))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a stored chunk's frame doesn't decode")));
	frame_plain(chunk.data, chunk.len, out);
	pfree(chunk.data);
}

void
prolly_chunk_frame(const void *data, Size len, StringInfo out)
{
	char		head[PROLLY_FRAME_HEADER];

	if (len > PG_UINT32_MAX)
		elog(ERROR, "chunk of %zu bytes is too large to frame", len);
	head[1] = len & 0xff;
	head[2] = (len >> 8) & 0xff;
	head[3] = (len >> 16) & 0xff;
	head[4] = (len >> 24) & 0xff;
#ifdef USE_LZ4
	/* (one whose bound is past what a buffer takes is framed as it is) */
	if (len > 0 && len <= LZ4_MAX_INPUT_SIZE &&
		(Size) LZ4_compressBound((int) len) + PROLLY_FRAME_HEADER <
		MaxAllocSize - (Size) out->len)
	{
		int			bound = LZ4_compressBound((int) len);
		int			n;

		enlargeStringInfo(out, PROLLY_FRAME_HEADER + bound);
		n = LZ4_compress_default(data, out->data + out->len + PROLLY_FRAME_HEADER,
								 (int) len, bound);
		if (n > 0 && (Size) n < len)
		{
			head[0] = PROLLY_CODEC_LZ4;
			memcpy(out->data + out->len, head, PROLLY_FRAME_HEADER);
			out->len += PROLLY_FRAME_HEADER + n;
			out->data[out->len] = '\0';
			return;
		}
	}
#endif
	head[0] = PROLLY_CODEC_NONE;
	appendBinaryStringInfo(out, head, PROLLY_FRAME_HEADER);
	appendBinaryStringInfo(out, data, len);
}

bool
prolly_chunk_frame_len(const char *frame, Size len, uint32 *chunk_len)
{
	const uint8 *p = (const uint8 *) frame;

	if (len < PROLLY_FRAME_HEADER)
		return false;
	*chunk_len = p[1] | (p[2] << 8) | (p[3] << 16) | ((uint32) p[4] << 24);
	if (p[0] == PROLLY_CODEC_NONE)
		return len - PROLLY_FRAME_HEADER == *chunk_len;
	return p[0] == PROLLY_CODEC_LZ4;
}

bool
prolly_chunk_unframe(const char *frame, Size len, StringInfo out)
{
	uint32		chunk_len;

	if (!prolly_chunk_frame_len(frame, len, &chunk_len))
		return false;
	if ((uint8) frame[0] == PROLLY_CODEC_NONE)
	{
		appendBinaryStringInfo(out, frame + PROLLY_FRAME_HEADER, chunk_len);
		return true;
	}
#ifdef USE_LZ4
	{
		int			n;

		/*
		 * (LZ4 makes nothing smaller than about a 255th of itself: a length
		 * past that, from a remote, isn't allocated for)
		 */
		if (chunk_len > MaxAllocSize - 1 - (Size) out->len ||
			chunk_len > (uint64) (len - PROLLY_FRAME_HEADER) * 255 + 16)
			return false;
		enlargeStringInfo(out, (int) chunk_len);
		n = LZ4_decompress_safe(frame + PROLLY_FRAME_HEADER, out->data + out->len,
								(int) (len - PROLLY_FRAME_HEADER), (int) chunk_len);
		if (n < 0 || (uint32) n != chunk_len)
			return false;
		out->len += n;
		out->data[out->len] = '\0';
		return true;
	}
#else
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("a chunk is compressed with LZ4, which this server was built without")));
	return false;
#endif
}

void
prolly_tablefile_read_stored(const ProllyTableFileRoutine *fmt, ProllyTableFile *tf,
							 const char *bytes, uint32 len, StringInfo out)
{
	StringInfoData data;

	if (PROLLY_HAS(fmt, read_stored))
	{
		PROLLY_CALL(fmt, read_stored, tf, bytes, len, out);
		return;
	}
	initStringInfo(&data);
	PROLLY_CALL(fmt, decode, tf, bytes, len, &data);
	prolly_chunk_frame(data.data, data.len, out);
	pfree(data.data);
}

void
prolly_tablefile_add_stored(const ProllyTableFileRoutine *fmt,
							ProllyTableFileWriter *w, const ProllyHash *hash,
							const char *frame, Size len, StringInfo out)
{
	StringInfoData chunk;

	if (PROLLY_HAS(fmt, write_add_stored))
	{
		PROLLY_CALL(fmt, write_add_stored, w, hash, frame, len, out);
		return;
	}
	initStringInfo(&chunk);
	if (!prolly_chunk_unframe(frame, len, &chunk))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s isn't framed as stored chunks are",
						prolly_hash_hex(hash))));
	PROLLY_CALL(fmt, write_add, w, hash, chunk.data, chunk.len, out);
	pfree(chunk.data);
}

bool
prolly_chunk_get_stored(const ProllyChunkStoreRoutine *cs, ProllyChunkStore *store,
						const ProllyHash *hash, StringInfo out)
{
	StringInfoData data;
	bool		found;

	if (prolly_serve_uncompressed)
	{
		/* (for a receiver that can't decompress: every frame plain) */
		initStringInfo(&data);
		found = PROLLY_CALL(cs, get, store, hash, &data);
		if (found)
			frame_plain(data.data, data.len, out);
		pfree(data.data);
		return found;
	}
	if (PROLLY_HAS(cs, get_stored))
		return PROLLY_CALL(cs, get_stored, store, hash, out);
	initStringInfo(&data);
	found = PROLLY_CALL(cs, get, store, hash, &data);
	if (found)
		prolly_chunk_frame(data.data, data.len, out);
	pfree(data.data);
	return found;
}

/* add_table_file()'s source for a store without add_stored_table_file() */
typedef struct Unframing
{
	ProllyChunkNext next;
	void	   *arg;
	StringInfoData frame;
} Unframing;

static bool
unframed_next(void *arg, ProllyHash *hash, StringInfo data)
{
	Unframing  *u = arg;

	resetStringInfo(&u->frame);
	if (!u->next(u->arg, hash, &u->frame))
		return false;
	if (!prolly_chunk_unframe(u->frame.data, u->frame.len, data))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s isn't framed as stored chunks are",
						prolly_hash_hex(hash))));
	return true;
}

void
prolly_chunk_add_stored(const ProllyChunkStoreRoutine *cs, ProllyChunkStore *store,
						ProllyChunkNext next, void *arg)
{
	Unframing	u = {next, arg};

	if (PROLLY_HAS(cs, add_stored_table_file))
	{
		PROLLY_CALL(cs, add_stored_table_file, store, next, arg);
		return;
	}
	initStringInfo(&u.frame);
	PROLLY_CALL(cs, add_table_file, store, unframed_next, &u);
	pfree(u.frame.data);
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

/* Is the chunk a leaf (a node of level 0, or not a node at all)? */
bool
prolly_chunk_is_leaf(const void *data, Size len)
{
	static const ProllyChunkFormatRoutine *format = NULL;
	ProllyChunkNode node;

	if (format == NULL)
		format = prolly_chunkformat_lookup(CHUNK_FORMAT_IMPL);
	if (!PROLLY_CALL(format, open, (const uint8 *) data, len, &node))
		return true;
	return node.level == 0;
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
