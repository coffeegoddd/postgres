/*-------------------------------------------------------------------------
 *
 * tablefile_basic.c
 *	  Table file format implementation "basic".
 *
 * A file is its chunks, then an index, then a footer:
 *
 *		chunk	uint8 codec (0: as is, 1: LZ4), uint32 length of the chunk,
 *				the bytes stored, uint32 CRC-32C of everything before it
 *		index	laid out as Dolt's table files' are, but for offsets where
 *				Dolt has lengths (in structure: Dolt's tools can't read
 *				these files, whose integers, codecs and footer are this
 *				format's own): for each chunk in hash order, its hash's
 *				first 8 bytes (a big-endian uint64, the prefix) and its
 *				ordinal (uint32: its place among the chunks); then for each
 *				chunk by ordinal, the uint64 offset of its bytes in the
 *				file (each ends where the next begins, the last where the
 *				index does); then for each by ordinal, the rest of its hash
 *				(the suffix); then a Bloom filter of the hashes,
 *				FILTER_BITS bits a chunk (a multiple of 64 in all)
 *		footer	uint64 offset of the index, uint32 chunk count, uint8 hash
 *				length, three zero bytes, uint32 CRC-32C of the index,
 *				uint32 version (3), "PRLYTBL1"
 *
 * integers little-endian, but the prefixes.  A chunk is compressed with LZ4
 * when the server is built with it and that makes the chunk smaller; a
 * server built without LZ4 reads the other chunks and refuses those.
 *
 * A reader uses the index where the file has it: mapped, if the opener can
 * map it (so every process shares the one copy in the OS's page cache),
 * or else read into memory, about 34 bytes a chunk for 20-byte hashes.
 * Offsets, not lengths, so nothing is computed from it.  A lookup tests
 * the filter first, so a hash the file lacks is almost always answered by
 * FILTER_PROBES bits; then it interpolates on the prefixes, which are
 * uniform (every other step bisects, so prefixes that aren't still take
 * O(log n) steps), and compares the suffixes of the chunks sharing its
 * prefix.
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

#define FORMAT_VERSION	3
#define MAGIC			"PRLYTBL1"
#define FOOTER_SIZE		32
#define CHUNK_HEADER	5		/* codec, length */
#define CHUNK_OVERHEAD	(CHUNK_HEADER + 4)
#define PREFIX_LEN		8		/* a hash's bytes in its prefix */
#define PREFIX_ENTRY	12		/* a prefix and an ordinal */
#define FILTER_BITS		10		/* a Bloom filter's bits a chunk */
#define FILTER_PROBES	7		/* and the bits each hash sets */

#define CODEC_NONE		0
#define CODEC_LZ4		1

/* a chunk the writer added: its hash, and where its bytes are */
typedef struct Entry
{
	ProllyHash	hash;
	uint32		ordinal;		/* the order it was added in */
	uint64		offset;
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

/* the index's sections, where they are: mapped, or in copy */
struct ProllyTableFile
{
	int			n;
	uint8		hash_len;
	int			suffix_len;		/* hash_len past the prefix */
	uint64		index_offset;	/* (where the last chunk ends) */
	const char *prefixes;		/* in hash order: prefix and ordinal each */
	const char *offsets;		/* by ordinal */
	const char *suffixes;		/* by ordinal, suffix_len bytes each */
	const char *filter;
	uint64		filter_bits;
	char	   *copy;			/* the index, if read rather than mapped */
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
	w->entries[w->n].offset = w->offset;
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

/* a filter of n chunks' size in bits */
static uint64
filter_size(uint32 n)
{
	return Max(((uint64) n * FILTER_BITS + 63) / 64 * 64, 64);
}

/*
 * The filter bits a hash sets: by double hashing on its own bytes, which a
 * content hash makes uniform (its first 8, and the 8 after, or its first 8
 * turned over when it is shorter).
 */
static void
filter_probes(const uint8 *bytes, uint8 hash_len, uint64 nbits,
			  uint64 *probes)
{
	uint64		h1 = hash_prefix(bytes, hash_len);
	uint64		h2 = hash_len >= 2 * PREFIX_LEN ?
		hash_prefix(bytes + PREFIX_LEN, PREFIX_LEN) :
		(h1 >> 29) ^ (h1 << 35) ^ UINT64CONST(0x9e3779b97f4a7c15);

	h2 |= 1;
	for (int i = 0; i < FILTER_PROBES; i++)
		probes[i] = (h1 + (uint64) i * h2) % nbits;
}

static void
basic_write_finish(ProllyTableFileWriter *w, StringInfo out)
{
	uint64		index_offset = w->offset;
	int			index_start = out->len;
	int			suffix_len = Max(w->hash_len - PREFIX_LEN, 0);
	uint64	   *offsets;
	uint64		nbits = filter_size(w->n);
	uint8	   *filter;
	char		footer[FOOTER_SIZE];
	pg_crc32c	crc;

	/* offsets and suffixes are by ordinal: the order the chunks came in */
	offsets = palloc_extended(sizeof(uint64) * Max(w->n, 1), MCXT_ALLOC_HUGE);
	for (int i = 0; i < w->n; i++)
		offsets[i] = w->entries[i].offset;
	filter = palloc_extended(nbits / 8, MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	if (w->n > 1)
		qsort(w->entries, w->n, sizeof(Entry), entry_cmp);
	for (int i = 0; i < w->n; i++)
	{
		char		pre[PREFIX_ENTRY];
		uint64		probes[FILTER_PROBES];

		if (i > 0 && hash_cmp(&w->entries[i - 1].hash, &w->entries[i].hash) == 0)
			elog(ERROR, "a chunk added to a table file twice");
		put_prefix(pre, hash_prefix(w->entries[i].hash.bytes, w->hash_len));
		put_u32(pre + PREFIX_LEN, w->entries[i].ordinal);
		appendBinaryStringInfo(out, pre, sizeof(pre));
		filter_probes(w->entries[i].hash.bytes, w->hash_len, nbits, probes);
		for (int p = 0; p < FILTER_PROBES; p++)
			filter[probes[p] / 8] |= (uint8) (1 << (probes[p] % 8));
	}
	for (int i = 0; i < w->n; i++)
	{
		char		off[8];

		put_u64(off, offsets[i]);
		appendBinaryStringInfo(out, off, sizeof(off));
	}
	/* (each suffix by ordinal: entries[] is in hash order now) */
	enlargeStringInfo(out, (Size) w->n * suffix_len);
	for (int i = 0; i < w->n; i++)
		memcpy(out->data + out->len + (Size) w->entries[i].ordinal * suffix_len,
			   w->entries[i].hash.bytes + PREFIX_LEN, suffix_len);
	out->len += w->n * suffix_len;
	out->data[out->len] = '\0';
	appendBinaryStringInfo(out, (char *) filter, nbits / 8);
	pfree(offsets);
	pfree(filter);
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
	if (tf->copy)
		pfree(tf->copy);
	pfree(tf);
}

/* chunk i (in hash order): its prefix, ordinal and suffix */
static uint64
prefix_at(ProllyTableFile *tf, int i)
{
	return get_prefix(tf->prefixes + (Size) i * PREFIX_ENTRY);
}

static uint32
ordinal_at(ProllyTableFile *tf, int i)
{
	return get_u32(tf->prefixes + (Size) i * PREFIX_ENTRY + PREFIX_LEN);
}

static const char *
suffix_at(ProllyTableFile *tf, int i)
{
	return tf->suffixes + (Size) ordinal_at(tf, i) * tf->suffix_len;
}

/* where the chunk of ordinal ord is in the file */
static void
chunk_at(ProllyTableFile *tf, uint32 ord, uint64 *offset, uint32 *len)
{
	uint64		start = get_u64(tf->offsets + (Size) ord * 8);
	uint64		end = ord + 1 < (uint32) tf->n ?
		get_u64(tf->offsets + (Size) (ord + 1) * 8) : tf->index_offset;

	*offset = start;
	*len = (uint32) (end - start);
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
basic_open(ProllyTableFileRead read, ProllyTableFileMap map, void *arg,
		   uint64 size, MemoryContext cxt)
{
	char		footer[FOOTER_SIZE];
	uint64		index_offset;
	uint32		n;
	uint8		hash_len;
	int			suffix_len;
	uint64		nbits;
	Size		index_len;
	const char *index;
	char	   *copy = NULL;
	pg_crc32c	crc;
	ProllyTableFile *tf;
	uint8	   *seen;
	uint64		prev_end = 0;

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
	nbits = filter_size(n);
	index_len = (Size) n * (PREFIX_ENTRY + 8 + suffix_len) + nbits / 8;
	if (index_offset > size - FOOTER_SIZE ||
		size - FOOTER_SIZE - index_offset != index_len)
		return NULL;
	index = map ? map(arg, index_offset, index_len) : NULL;
	if (index == NULL)
	{
		copy = MemoryContextAllocHuge(cxt, Max(index_len, 1));
		if (!read(arg, index_offset, copy, index_len))
		{
			pfree(copy);
			return NULL;
		}
		index = copy;
	}
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, index, index_len);
	FIN_CRC32C(crc);
	if (!EQ_CRC32C(crc, get_u32(footer + 16)))
	{
		if (copy)
			pfree(copy);
		return NULL;
	}

	tf = MemoryContextAllocZero(cxt, sizeof(*tf));
	tf->n = n;
	tf->hash_len = hash_len;
	tf->suffix_len = suffix_len;
	tf->index_offset = index_offset;
	tf->prefixes = index;
	tf->offsets = index + (Size) n * PREFIX_ENTRY;
	tf->suffixes = tf->offsets + (Size) n * 8;
	tf->filter = tf->suffixes + (Size) n * suffix_len;
	tf->filter_bits = nbits;
	tf->copy = copy;

	/* each ordinal once, the chunks one after another, the hashes in order */
	seen = palloc_extended(Max(n / 8 + 1, 1), MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	for (uint32 i = 0; i < n; i++)
	{
		uint32		ord = ordinal_at(tf, i);

		if (ord >= n || (seen[ord / 8] & (1 << (ord % 8))))
			goto bad;
		seen[ord / 8] |= (uint8) (1 << (ord % 8));
		if (i > 0 && (prefix_at(tf, i - 1) > prefix_at(tf, i) ||
					  (prefix_at(tf, i - 1) == prefix_at(tf, i) &&
					   memcmp(suffix_at(tf, i - 1), suffix_at(tf, i),
							  suffix_len) >= 0)))
			goto bad;
	}
	for (uint32 ord = 0; ord < n; ord++)
	{
		uint64		offset = get_u64(tf->offsets + (Size) ord * 8);
		uint64		end = ord + 1 < n ?
			get_u64(tf->offsets + (Size) (ord + 1) * 8) : index_offset;

		if (offset != prev_end || end < offset ||
			end - offset < CHUNK_OVERHEAD || end - offset > PG_UINT32_MAX)
			goto bad;
		prev_end = end;
	}
	if (prev_end != index_offset)
		goto bad;
	pfree(seen);
	return tf;

bad:
	pfree(seen);
	basic_close(tf);
	return NULL;
}

/* Might the file have hash?  False: certainly not. */
static bool
filter_has(ProllyTableFile *tf, const ProllyHash *hash)
{
	uint64		probes[FILTER_PROBES];

	filter_probes(hash->bytes, hash->len, tf->filter_bits, probes);
	for (int p = 0; p < FILTER_PROBES; p++)
	{
		if (!(((const uint8 *) tf->filter)[probes[p] / 8] & (1 << (probes[p] % 8))))
			return false;
	}
	return true;
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
		uint64		plo = prefix_at(tf, lo);
		uint64		phi = prefix_at(tf, hi);
		uint64		pmid;
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
		pmid = prefix_at(tf, mid);
		if (pmid < prefix)
			lo = mid + 1;
		else if (pmid > prefix)
			hi = mid - 1;
		else
		{
			while (mid > 0 && prefix_at(tf, mid - 1) == prefix)
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

	if (tf->n == 0 || hash->len != tf->hash_len || !filter_has(tf, hash))
		return -1;
	prefix = hash_prefix(hash->bytes, hash->len);
	for (i = find_prefix(tf, prefix); i >= 0 && i < tf->n &&
		 prefix_at(tf, i) == prefix; i++)
	{
		int			c = memcmp(suffix_at(tf, i), hash->bytes + PREFIX_LEN,
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

	if (i < 0)
		return false;
	chunk_at(tf, ordinal_at(tf, i), offset, len);
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
	char		pre[PREFIX_LEN];

	if (i < 0 || i >= tf->n)
		elog(ERROR, "table file entry %d out of range", i);
	memset(hash, 0, sizeof(*hash));
	hash->len = tf->hash_len;
	put_prefix(pre, prefix_at(tf, i));
	memcpy(hash->bytes, pre, Min(tf->hash_len, PREFIX_LEN));
	memcpy(hash->bytes + PREFIX_LEN, suffix_at(tf, i), tf->suffix_len);
	chunk_at(tf, ordinal_at(tf, i), offset, len);
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
