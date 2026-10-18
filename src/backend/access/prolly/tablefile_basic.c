/*-------------------------------------------------------------------------
 *
 * tablefile_basic.c
 *	  Table file format implementation "basic".
 *
 * A file is its chunks, then an index, then a footer:
 *
 *		chunk	uint8 codec (0: as is, 1: LZ4), uint32 length of the chunk,
 *				the bytes stored, uint32 CRC-32C of everything before it
 *		index	for each chunk, in hash order: its hash, uint64 offset and
 *				uint32 length of its bytes in the file
 *		footer	uint64 offset of the index, uint32 chunk count, uint8 hash
 *				length, three zero bytes, uint32 CRC-32C of the index,
 *				uint32 version (1), "PRLYTBL1"
 *
 * integers little-endian.  A chunk is compressed with LZ4 when the server
 * is built with it and that makes the chunk smaller; a server built
 * without LZ4 reads the other chunks and refuses those.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/tablefile_basic.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LZ4
#include <lz4.h>
#endif

#include "access/prolly_builtins.h"
#include "port/pg_crc32c.h"

#define FORMAT_VERSION	1
#define MAGIC			"PRLYTBL1"
#define FOOTER_SIZE		32
#define CHUNK_HEADER	5		/* codec, length */
#define CHUNK_OVERHEAD	(CHUNK_HEADER + 4)

#define CODEC_NONE		0
#define CODEC_LZ4		1

typedef struct Entry
{
	ProllyHash	hash;
	uint64		offset;
	uint32		len;
} Entry;

struct ProllyTableFileWriter
{
	MemoryContext cxt;
	Entry	   *entries;
	int			n;
	int			max;
	uint64		offset;			/* the file's length so far */
	uint8		hash_len;
};

struct ProllyTableFile
{
	Entry	   *entries;		/* in hash order */
	int			n;
};

static void
put_u32(char *p, uint32 v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static void
put_u64(char *p, uint64 v)
{
	put_u32(p, (uint32) v);
	put_u32(p + 4, (uint32) (v >> 32));
}

static uint32
get_u32(const char *p)
{
	const uint8 *u = (const uint8 *) p;

	return (uint32) u[0] | ((uint32) u[1] << 8) | ((uint32) u[2] << 16) |
		((uint32) u[3] << 24);
}

static uint64
get_u64(const char *p)
{
	return (uint64) get_u32(p) | ((uint64) get_u32(p + 4) << 32);
}

static int
hash_cmp(const ProllyHash *a, const ProllyHash *b)
{
	int			r = memcmp(a->bytes, b->bytes, Min(a->len, b->len));

	return r != 0 ? r : (a->len > b->len) - (a->len < b->len);
}

static int
entry_cmp(const void *a, const void *b)
{
	return hash_cmp(&((const Entry *) a)->hash, &((const Entry *) b)->hash);
}

static ProllyTableFileWriter *
basic_write_begin(MemoryContext cxt)
{
	ProllyTableFileWriter *w = MemoryContextAllocZero(cxt, sizeof(*w));

	w->cxt = cxt;
	return w;
}

static void
basic_write_add(ProllyTableFileWriter *w, const ProllyHash *hash,
				const void *data, Size len, StringInfo out)
{
	int			start = out->len;
	char		head[CHUNK_HEADER];
	char		tail[4];
	const char *stored = data;
	Size		stored_len = len;
	char	   *compressed = NULL;
	uint8		codec = CODEC_NONE;
	pg_crc32c	crc;

	if (hash->len == 0 || (w->hash_len != 0 && hash->len != w->hash_len))
		elog(ERROR, "table file chunk hash of %u bytes", hash->len);
	if (len > PG_UINT32_MAX - CHUNK_OVERHEAD)
		elog(ERROR, "chunk of %zu bytes is too large for a table file", len);
	w->hash_len = hash->len;

#ifdef USE_LZ4
	if (len > 0)
	{
		int			bound = LZ4_compressBound((int) len);
		int			n;

		compressed = palloc(bound);
		n = LZ4_compress_default(data, compressed, (int) len, bound);
		if (n > 0 && (Size) n < len)
		{
			stored = compressed;
			stored_len = n;
			codec = CODEC_LZ4;
		}
	}
#endif

	head[0] = (char) codec;
	put_u32(head + 1, (uint32) len);
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, head, CHUNK_HEADER);
	COMP_CRC32C(crc, stored, stored_len);
	FIN_CRC32C(crc);
	put_u32(tail, crc);
	appendBinaryStringInfo(out, head, CHUNK_HEADER);
	appendBinaryStringInfo(out, stored, stored_len);
	appendBinaryStringInfo(out, tail, 4);
	if (compressed)
		pfree(compressed);

	if (w->n == w->max)
	{
		w->max = Max(64, w->max * 2);
		w->entries = w->entries ?
			repalloc_huge(w->entries, sizeof(Entry) * w->max) :
			MemoryContextAllocHuge(w->cxt, sizeof(Entry) * w->max);
	}
	w->entries[w->n].hash = *hash;
	w->entries[w->n].offset = w->offset;
	w->entries[w->n].len = out->len - start;
	w->n++;
	w->offset += out->len - start;
}

static void
basic_write_finish(ProllyTableFileWriter *w, StringInfo out)
{
	uint64		index_offset = w->offset;
	int			index_start = out->len;
	char		footer[FOOTER_SIZE];
	pg_crc32c	crc;

	if (w->n > 1)
		qsort(w->entries, w->n, sizeof(Entry), entry_cmp);
	for (int i = 0; i < w->n; i++)
	{
		char		loc[12];

		if (i > 0 && hash_cmp(&w->entries[i - 1].hash, &w->entries[i].hash) == 0)
			elog(ERROR, "a chunk added to a table file twice");
		put_u64(loc, w->entries[i].offset);
		put_u32(loc + 8, w->entries[i].len);
		appendBinaryStringInfo(out, w->entries[i].hash.bytes, w->hash_len);
		appendBinaryStringInfo(out, loc, sizeof(loc));
	}
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, out->data + index_start, out->len - index_start);
	FIN_CRC32C(crc);

	memset(footer, 0, sizeof(footer));
	put_u64(footer, index_offset);
	put_u32(footer + 8, (uint32) w->n);
	footer[12] = (char) w->hash_len;
	put_u32(footer + 16, crc);
	put_u32(footer + 20, FORMAT_VERSION);
	memcpy(footer + 24, MAGIC, 8);
	appendBinaryStringInfo(out, footer, sizeof(footer));

	if (w->entries)
		pfree(w->entries);
	pfree(w);
}

static ProllyTableFile *
basic_open(ProllyTableFileRead read, void *arg, uint64 size, MemoryContext cxt)
{
	char		footer[FOOTER_SIZE];
	uint64		index_offset;
	uint32		n;
	uint8		hash_len;
	Size		entry_size;
	Size		index_len;
	char	   *index;
	pg_crc32c	crc;
	ProllyTableFile *tf;

	if (size < FOOTER_SIZE || !read(arg, size - FOOTER_SIZE, footer, FOOTER_SIZE) ||
		memcmp(footer + 24, MAGIC, 8) != 0 ||
		get_u32(footer + 20) != FORMAT_VERSION)
		return NULL;
	index_offset = get_u64(footer);
	n = get_u32(footer + 8);
	hash_len = (uint8) footer[12];
	/* (a file of no chunks has no hash length) */
	if ((hash_len == 0) != (n == 0) || hash_len > PROLLY_HASH_MAX_LEN)
		return NULL;
	entry_size = hash_len + 12;
	if (index_offset > size - FOOTER_SIZE ||
		(size - FOOTER_SIZE - index_offset) / entry_size != n ||
		(size - FOOTER_SIZE - index_offset) % entry_size != 0)
		return NULL;
	index_len = (Size) n * entry_size;
	index = palloc_extended(Max(index_len, 1), MCXT_ALLOC_HUGE);
	if (index_len > 0 && !read(arg, index_offset, index, index_len))
	{
		pfree(index);
		return NULL;
	}
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, index, index_len);
	FIN_CRC32C(crc);
	if (!EQ_CRC32C(crc, get_u32(footer + 16)))
	{
		pfree(index);
		return NULL;
	}

	tf = MemoryContextAllocZero(cxt, sizeof(*tf));
	tf->n = n;
	tf->entries = MemoryContextAllocHuge(cxt, sizeof(Entry) * Max(n, 1));
	for (uint32 i = 0; i < n; i++)
	{
		const char *e = index + (Size) i * entry_size;
		Entry	   *en = &tf->entries[i];

		memset(&en->hash, 0, sizeof(en->hash));
		en->hash.len = hash_len;
		memcpy(en->hash.bytes, e, hash_len);
		en->offset = get_u64(e + hash_len);
		en->len = get_u32(e + hash_len + 8);
		if (en->len < CHUNK_OVERHEAD || en->offset > index_offset ||
			en->len > index_offset - en->offset ||
			(i > 0 && hash_cmp(&tf->entries[i - 1].hash, &en->hash) >= 0))
		{
			pfree(index);
			pfree(tf->entries);
			pfree(tf);
			return NULL;
		}
	}
	pfree(index);
	return tf;
}

static int
basic_locate(ProllyTableFile *tf, const ProllyHash *hash)
{
	int			lo = 0;
	int			hi = tf->n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;
		int			c = hash_cmp(&tf->entries[mid].hash, hash);

		if (c == 0)
			return mid;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

static bool
basic_find(ProllyTableFile *tf, const ProllyHash *hash, uint64 *offset,
		   uint32 *len)
{
	int			i = basic_locate(tf, hash);

	if (i < 0)
		return false;
	*offset = tf->entries[i].offset;
	*len = tf->entries[i].len;
	return true;
}

static void
basic_decode(ProllyTableFile *tf, const char *bytes, uint32 len, StringInfo out)
{
	pg_crc32c	crc;
	uint8		codec;
	uint32		raw_len;
	const char *stored = bytes + CHUNK_HEADER;
	uint32		stored_len;

	if (len < CHUNK_OVERHEAD)
		goto corrupt;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, bytes, len - 4);
	FIN_CRC32C(crc);
	if (!EQ_CRC32C(crc, get_u32(bytes + len - 4)))
		goto corrupt;
	codec = (uint8) bytes[0];
	raw_len = get_u32(bytes + 1);
	stored_len = len - CHUNK_OVERHEAD;

	if (codec == CODEC_NONE)
	{
		if (stored_len != raw_len)
			goto corrupt;
		appendBinaryStringInfo(out, stored, stored_len);
		return;
	}
	if (codec == CODEC_LZ4)
	{
#ifdef USE_LZ4
		int			n;

		enlargeStringInfo(out, raw_len);
		n = LZ4_decompress_safe(stored, out->data + out->len, (int) stored_len,
								(int) raw_len);
		if (n < 0 || (uint32) n != raw_len)
			goto corrupt;
		out->len += raw_len;
		out->data[out->len] = '\0';
		return;
#else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a table file chunk is compressed with LZ4, which this server was built without")));
#endif
	}

corrupt:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("corrupt chunk in a table file")));
}

static int
basic_count(ProllyTableFile *tf)
{
	return tf->n;
}

static void
basic_entry(ProllyTableFile *tf, int i, ProllyHash *hash, uint64 *offset,
			uint32 *len)
{
	if (i < 0 || i >= tf->n)
		elog(ERROR, "table file entry %d out of range", i);
	*hash = tf->entries[i].hash;
	*offset = tf->entries[i].offset;
	*len = tf->entries[i].len;
}

static void
basic_close(ProllyTableFile *tf)
{
	pfree(tf->entries);
	pfree(tf);
}

const ProllyTableFileRoutine prolly_tablefile_basic = {
	.info = {
		.iface = PROLLY_IFACE_TABLE_FILE,
		.impl_id = 1,
		.format_version = FORMAT_VERSION,
		.name = "basic",
		.description = "chunks, LZ4-compressed where that makes them smaller, "
		"then their index by hash",
	},
	.write_begin = basic_write_begin,
	.write_add = basic_write_add,
	.write_finish = basic_write_finish,
	.open = basic_open,
	.find = basic_find,
	.decode = basic_decode,
	.count = basic_count,
	.entry = basic_entry,
	.locate = basic_locate,
	.close = basic_close,
};
