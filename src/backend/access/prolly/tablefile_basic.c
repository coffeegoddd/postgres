/*-------------------------------------------------------------------------
 *
 * tablefile_basic.c
 *	  Table file format implementation "basic".
 *
 * A file is its chunks, then an index, then a footer:
 *
 *		chunk	uint8 codec (0: as is, 1: LZ4), uint32 length of the chunk,
 *				the bytes stored, uint32 CRC-32C of everything before it
 *		index	laid out as Dolt's table files' are (in structure: Dolt's
 *				tools can't read these files, whose integers, codecs and
 *				footer are this format's own): for each chunk in hash
 *				order, its hash's first 8 bytes (a big-endian uint64, the
 *				prefix) and its ordinal (uint32: its place among the
 *				chunks); then for each chunk by ordinal, the uint32 length
 *				of its bytes in the file; then for each by ordinal, the
 *				rest of its hash (the suffix)
 *		footer	uint64 offset of the index, uint32 chunk count, uint8 hash
 *				length, three zero bytes, uint32 CRC-32C of the index,
 *				uint32 version (2), "PRLYTBL1"
 *
 * integers little-endian, but the prefixes.  A chunk is compressed with LZ4
 * when the server is built with it and that makes the chunk smaller; a
 * server built without LZ4 reads the other chunks and refuses those.
 *
 * A reader keeps the index as the file has it, the lengths made offsets:
 * about 32 bytes a chunk for 20-byte hashes.  A lookup interpolates on the
 * prefixes, which are uniform (every other step bisects, so prefixes that
 * aren't still take O(log n) steps), then compares the suffixes of the
 * chunks with its prefix.
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
#include "utils/memutils.h"

#define FORMAT_VERSION	2
#define MAGIC			"PRLYTBL1"
#define FOOTER_SIZE		32
#define CHUNK_HEADER	5		/* codec, length */
#define CHUNK_OVERHEAD	(CHUNK_HEADER + 4)
#define PREFIX_LEN		8		/* a hash's bytes in its prefix */
#define PREFIX_ENTRY	12		/* a prefix and an ordinal */

#define CODEC_NONE		0
#define CODEC_LZ4		1

/* a chunk the writer added: its hash, and where its bytes are */
typedef struct Entry
{
	ProllyHash	hash;
	uint32		ordinal;		/* the order it was added in */
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
	int			n;
	uint8		hash_len;
	int			suffix_len;		/* hash_len past the prefix */
	uint64	   *prefixes;		/* in hash order */
	uint32	   *ordinals;		/* likewise: each one's place in the file */
	uint64	   *offsets;		/* by ordinal, and the chunks' end last */
	uint8	   *suffixes;		/* by ordinal, suffix_len bytes each */
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
	/* (one whose bound is past what palloc gives is stored as is) */
	if (len > 0 && len <= LZ4_MAX_INPUT_SIZE &&
		(Size) LZ4_compressBound((int) len) <= MaxAllocSize)
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
	w->entries[w->n].ordinal = (uint32) w->n;
	w->entries[w->n].len = out->len - start;
	w->n++;
	w->offset += out->len - start;
}

/* A hash's prefix: its first bytes, as a big-endian number. */
static uint64
hash_prefix(const uint8 *bytes, uint8 hash_len)
{
	uint64		v = 0;

	for (int i = 0; i < PREFIX_LEN; i++)
		v = (v << 8) | (i < hash_len ? bytes[i] : 0);
	return v;
}

static void
put_prefix(char *p, uint64 v)
{
	for (int i = PREFIX_LEN - 1; i >= 0; i--)
	{
		p[i] = (char) (v & 0xff);
		v >>= 8;
	}
}

static uint64
get_prefix(const char *p)
{
	return hash_prefix((const uint8 *) p, PREFIX_LEN);
}

static void
basic_write_finish(ProllyTableFileWriter *w, StringInfo out)
{
	uint64		index_offset = w->offset;
	int			index_start = out->len;
	int			suffix_len = Max(w->hash_len - PREFIX_LEN, 0);
	uint32	   *lens;
	char		footer[FOOTER_SIZE];
	pg_crc32c	crc;

	/* lengths and suffixes are by ordinal: the order the chunks came in */
	lens = palloc_extended(sizeof(uint32) * Max(w->n, 1), MCXT_ALLOC_HUGE);
	for (int i = 0; i < w->n; i++)
		lens[i] = w->entries[i].len;
	if (w->n > 1)
		qsort(w->entries, w->n, sizeof(Entry), entry_cmp);
	for (int i = 0; i < w->n; i++)
	{
		char		pre[PREFIX_ENTRY];

		if (i > 0 && hash_cmp(&w->entries[i - 1].hash, &w->entries[i].hash) == 0)
			elog(ERROR, "a chunk added to a table file twice");
		put_prefix(pre, hash_prefix(w->entries[i].hash.bytes, w->hash_len));
		put_u32(pre + PREFIX_LEN, w->entries[i].ordinal);
		appendBinaryStringInfo(out, pre, sizeof(pre));
	}
	for (int i = 0; i < w->n; i++)
	{
		char		len[4];

		put_u32(len, lens[i]);
		appendBinaryStringInfo(out, len, sizeof(len));
	}
	/* (each suffix by ordinal: entries[] is in hash order now) */
	enlargeStringInfo(out, (Size) w->n * suffix_len);
	for (int i = 0; i < w->n; i++)
		memcpy(out->data + out->len + (Size) w->entries[i].ordinal * suffix_len,
			   w->entries[i].hash.bytes + PREFIX_LEN, suffix_len);
	out->len += w->n * suffix_len;
	out->data[out->len] = '\0';
	pfree(lens);
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

static void
basic_close(ProllyTableFile *tf)
{
	if (tf->prefixes)
		pfree(tf->prefixes);
	if (tf->ordinals)
		pfree(tf->ordinals);
	if (tf->offsets)
		pfree(tf->offsets);
	if (tf->suffixes)
		pfree(tf->suffixes);
	pfree(tf);
}

/* chunk i's suffix, in hash order */
static const uint8 *
suffix_of(ProllyTableFile *tf, int i)
{
	return tf->suffixes + (Size) tf->ordinals[i] * tf->suffix_len;
}

/*
 * Compare chunk i (in hash order) with a hash of the file's length and the
 * given prefix: the prefixes, then the suffixes.
 */
static int
cmp_at(ProllyTableFile *tf, int i, uint64 prefix, const uint8 *suffix)
{
	if (tf->prefixes[i] != prefix)
		return tf->prefixes[i] < prefix ? -1 : 1;
	return memcmp(suffix_of(tf, i), suffix, tf->suffix_len);
}

/* The version a file's footer says, if it is this format's (else 0). */
static uint32
basic_version(ProllyTableFileRead read, void *arg, uint64 size)
{
	char		footer[FOOTER_SIZE];

	if (size < FOOTER_SIZE || !read(arg, size - FOOTER_SIZE, footer, FOOTER_SIZE) ||
		memcmp(footer + 24, MAGIC, 8) != 0)
		return 0;
	return get_u32(footer + 20);
}

static ProllyTableFile *
basic_open(ProllyTableFileRead read, void *arg, uint64 size, MemoryContext cxt)
{
	char		footer[FOOTER_SIZE];
	uint64		index_offset;
	uint32		n;
	uint8		hash_len;
	int			suffix_len;
	Size		entry_size;
	Size		index_len;
	char	   *index;
	const char *p;
	pg_crc32c	crc;
	ProllyTableFile *tf;
	bool	   *seen;
	uint64		offset;

	if (size < FOOTER_SIZE || !read(arg, size - FOOTER_SIZE, footer, FOOTER_SIZE) ||
		memcmp(footer + 24, MAGIC, 8) != 0 ||
		get_u32(footer + 20) != FORMAT_VERSION)
		return NULL;
	index_offset = get_u64(footer);
	n = get_u32(footer + 8);
	hash_len = (uint8) footer[12];
	/* (a file of no chunks has no hash length) */
	if ((hash_len == 0) != (n == 0) || hash_len > PROLLY_HASH_MAX_LEN ||
		n > (uint32) PG_INT32_MAX)
		return NULL;
	suffix_len = Max(hash_len - PREFIX_LEN, 0);
	entry_size = PREFIX_ENTRY + 4 + suffix_len;
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
	tf->hash_len = hash_len;
	tf->suffix_len = suffix_len;
	tf->prefixes = MemoryContextAllocHuge(cxt, sizeof(uint64) * Max(n, 1));
	tf->ordinals = MemoryContextAllocHuge(cxt, sizeof(uint32) * Max(n, 1));
	tf->offsets = MemoryContextAllocHuge(cxt, sizeof(uint64) * (n + 1));
	tf->suffixes = MemoryContextAllocHuge(cxt, Max((Size) n * suffix_len, 1));
	seen = palloc_extended(Max(n, 1), MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);

	p = index;
	for (uint32 i = 0; i < n; i++, p += PREFIX_ENTRY)
	{
		tf->prefixes[i] = get_prefix(p);
		tf->ordinals[i] = get_u32(p + PREFIX_LEN);
		if (tf->ordinals[i] >= n || seen[tf->ordinals[i]])
			goto bad;
		seen[tf->ordinals[i]] = true;
	}
	offset = 0;
	for (uint32 i = 0; i < n; i++, p += 4)
	{
		uint32		len = get_u32(p);

		if (len < CHUNK_OVERHEAD || len > index_offset - offset)
			goto bad;
		tf->offsets[i] = offset;
		offset += len;
	}
	tf->offsets[n] = offset;
	if (offset != index_offset)
		goto bad;
	memcpy(tf->suffixes, p, (Size) n * suffix_len);
	/* in hash order, each once */
	for (uint32 i = 1; i < n; i++)
	{
		if (cmp_at(tf, i - 1, tf->prefixes[i], suffix_of(tf, i)) >= 0)
			goto bad;
	}
	pfree(seen);
	pfree(index);
	return tf;

bad:
	pfree(seen);
	pfree(index);
	basic_close(tf);
	return NULL;
}

/*
 * The first chunk (in hash order) whose prefix is prefix, or -1: by
 * interpolation, as the prefixes of content hashes are uniform, every
 * other step a bisection, so prefixes that aren't (a guess landing at an
 * end each time) cost O(log n) steps, not O(n).
 */
static int
find_prefix(ProllyTableFile *tf, uint64 prefix)
{
	int			lo = 0;
	int			hi = tf->n - 1;
	bool		bisect = false;

	while (lo <= hi)
	{
		uint64		plo = tf->prefixes[lo];
		uint64		phi = tf->prefixes[hi];
		int			mid;

		if (prefix < plo || prefix > phi)
			return -1;
		if (phi == plo)
			mid = lo;
		else if (bisect)
			mid = lo + (hi - lo) / 2;
		else
		{
			long double f = (long double) (prefix - plo) / (long double) (phi - plo);

			mid = lo + (int) (f * (hi - lo));
			mid = Max(lo, Min(hi, mid));
		}
		bisect = !bisect;
		if (tf->prefixes[mid] < prefix)
			lo = mid + 1;
		else if (tf->prefixes[mid] > prefix)
			hi = mid - 1;
		else
		{
			while (mid > 0 && tf->prefixes[mid - 1] == prefix)
				mid--;
			return mid;
		}
	}
	return -1;
}

static int
basic_locate(ProllyTableFile *tf, const ProllyHash *hash)
{
	uint64		prefix;
	int			i;

	if (tf->n == 0 || hash->len != tf->hash_len)
		return -1;
	prefix = hash_prefix(hash->bytes, hash->len);
	for (i = find_prefix(tf, prefix); i >= 0 && i < tf->n &&
		 tf->prefixes[i] == prefix; i++)
	{
		int			c = memcmp(suffix_of(tf, i), hash->bytes + PREFIX_LEN,
							   tf->suffix_len);

		if (c == 0)
			return i;
		if (c > 0)
			break;
	}
	return -1;
}

static bool
basic_find(ProllyTableFile *tf, const ProllyHash *hash, uint64 *offset,
		   uint32 *len)
{
	int			i = basic_locate(tf, hash);
	uint32		ord;

	if (i < 0)
		return false;
	ord = tf->ordinals[i];
	*offset = tf->offsets[ord];
	*len = (uint32) (tf->offsets[ord + 1] - tf->offsets[ord]);
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
	uint32		ord;
	char		pre[PREFIX_LEN];

	if (i < 0 || i >= tf->n)
		elog(ERROR, "table file entry %d out of range", i);
	ord = tf->ordinals[i];
	memset(hash, 0, sizeof(*hash));
	hash->len = tf->hash_len;
	put_prefix(pre, tf->prefixes[i]);
	memcpy(hash->bytes, pre, Min(tf->hash_len, PREFIX_LEN));
	memcpy(hash->bytes + PREFIX_LEN, suffix_of(tf, i), tf->suffix_len);
	*offset = tf->offsets[ord];
	*len = (uint32) (tf->offsets[ord + 1] - tf->offsets[ord]);
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
	.version = basic_version,
};
