/*-------------------------------------------------------------------------
 *
 * chunkformat_basic.c
 *	  Chunk node format implementation "basic".
 *
 * A node is
 *
 *		uint8 version (1), uint8 kind (1: a tree node), uint16 level,
 *		uint32 item count, uint32 offset of each item, then the items:
 *		varint key length, key, uint8 flags, varint value length, value
 *
 * integers little-endian, varints LEB128 (seven bits a byte, low first).
 * Every byte is canonical: the same level and items give the same bytes.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/chunkformat_basic.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

#define FORMAT_VERSION	1
#define KIND_NODE		1
#define HEADER_SIZE		8

static void
put_u16(StringInfo out, uint16 v)
{
	uint8		b[2] = {v & 0xff, v >> 8};

	appendBinaryStringInfo(out, b, 2);
}

static void
put_u32_at(char *p, uint32 v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static uint32
get_u32(const uint8 *p)
{
	return (uint32) p[0] | ((uint32) p[1] << 8) | ((uint32) p[2] << 16) |
		((uint32) p[3] << 24);
}

static void
put_varint(StringInfo out, uint64 v)
{
	do
	{
		uint8		b = v & 0x7f;

		v >>= 7;
		if (v != 0)
			b |= 0x80;
		appendStringInfoChar(out, (char) b);
	} while (v != 0);
}

static Size
varint_len(uint64 v)
{
	Size		n = 1;

	while (v >= 0x80)
	{
		v >>= 7;
		n++;
	}
	return n;
}

/* Read a varint at *p (before end); false if it runs past end. */
static bool
get_varint(const uint8 **p, const uint8 *end, uint64 *v)
{
	uint64		r = 0;
	int			shift = 0;

	while (*p < end && shift < 64)
	{
		uint8		b = *(*p)++;

		r |= (uint64) (b & 0x7f) << shift;
		if ((b & 0x80) == 0)
		{
			*v = r;
			return true;
		}
		shift += 7;
	}
	return false;
}

static void
basic_encode(uint16 level, int nitems, const ProllyKey *keys,
			 const ProllyValue *values, const uint8 *flags, StringInfo out)
{
	int			base = out->len;	/* offsets are from the chunk's start */
	int			table;

	appendStringInfoChar(out, (char) FORMAT_VERSION);
	appendStringInfoChar(out, (char) KIND_NODE);
	put_u16(out, level);
	enlargeStringInfo(out, 4);
	put_u32_at(out->data + out->len, (uint32) nitems);
	out->len += 4;
	table = out->len;
	for (int i = 0; i < nitems; i++)
		appendBinaryStringInfo(out, "\0\0\0\0", 4);
	for (int i = 0; i < nitems; i++)
	{
		put_u32_at(out->data + table + 4 * i, (uint32) (out->len - base));
		put_varint(out, keys[i].len);
		if (keys[i].len > 0)
			appendBinaryStringInfo(out, keys[i].data, keys[i].len);
		appendStringInfoChar(out, (char) (flags ? flags[i] : 0));
		put_varint(out, values[i].len);
		if (values[i].len > 0)
			appendBinaryStringInfo(out, values[i].data, values[i].len);
	}
	out->data[out->len] = '\0';
}

static bool
basic_open(const uint8 *data, Size len, ProllyChunkNode *node)
{
	uint32		n;
	Size		table_end;
	uint32		prev = 0;

	if (len < HEADER_SIZE || data[0] != FORMAT_VERSION || data[1] != KIND_NODE)
		return false;
	n = get_u32(data + 4);
	table_end = HEADER_SIZE + (Size) n * 4;
	if (n > len || table_end > len)
		return false;
	for (uint32 i = 0; i < n; i++)
	{
		uint32		off = get_u32(data + HEADER_SIZE + 4 * i);

		if (off < table_end || off >= len || (i > 0 && off <= prev))
			return false;
		prev = off;
	}
	node->data = data;
	node->len = len;
	node->level = (uint16) data[2] | ((uint16) data[3] << 8);
	node->nitems = (int) n;
	return true;
}

static void
basic_item_get(const ProllyChunkNode *node, int index, ProllyKey *key,
			   ProllyValue *value, uint8 *flags)
{
	const uint8 *end = node->data + node->len;
	const uint8 *p;
	uint64		klen;
	uint64		vlen;
	uint8		f;

	if (index < 0 || index >= node->nitems)
		elog(ERROR, "prolly chunk node item %d out of range", index);
	p = node->data + get_u32(node->data + HEADER_SIZE + 4 * index);
	if (!get_varint(&p, end, &klen) || klen > (uint64) (end - p))
		goto corrupt;
	key->data = p;
	key->len = (uint32) klen;
	p += klen;
	if (p >= end)
		goto corrupt;
	f = *p++;
	if (!get_varint(&p, end, &vlen) || vlen > (uint64) (end - p))
		goto corrupt;
	value->data = p;
	value->len = (uint32) vlen;
	if (flags)
		*flags = f;
	return;

corrupt:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("corrupt prolly chunk node item %d", index)));
}

static Size
basic_item_space(uint16 level, const ProllyKey *key, const ProllyValue *value)
{
	return 4 + varint_len(key->len) + key->len + 1 +
		varint_len(value->len) + value->len;
}

static Size
basic_header_size(void)
{
	return HEADER_SIZE;
}

const ProllyChunkFormatRoutine prolly_chunkformat_basic = {
	.info = {
		.iface = PROLLY_IFACE_CHUNK_FORMAT,
		.impl_id = 1,
		/*
		 * (a store's chunks' version, which a store records and refuses
		 * another of: 2 since items declare the chunks they name, which a
		 * collection follows alone, so a store whose chunks don't is
		 * refused; the node's own byte stays 1, as old nodes read as ever)
		 */
		.format_version = 2,
		.name = "basic",
		.description = "a version byte, level, item offsets, then keys, "
		"flags and values",
	},
	.encode = basic_encode,
	.open = basic_open,
	.item_get = basic_item_get,
	.item_space = basic_item_space,
	.header_size = basic_header_size,
};
