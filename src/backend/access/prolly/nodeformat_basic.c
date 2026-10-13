/*-------------------------------------------------------------------------
 *
 * nodeformat_basic.c
 *	  Node page format implementation "basic".
 *
 * Page layout:
 *
 *	+-----------------+--------------------------------+------+-------+---------+
 *	| PageHeaderData  | canonical region ->            | free | <- item | special |
 *	|                 | level, count, item, item, ...  |      | offsets |         |
 *	+-----------------+--------------------------------+------+-------+---------+
 *	                  ^                                ^      ^         ^
 *	        SizeOfPageHeaderData                   pd_lower pd_upper pd_special
 *
 * The canonical region is the bytes that are hashed.  It holds the level and
 * item count as big-endian uint16, then the items in order, each as
 *
 *	uint16 key length, uint16 value length (big-endian), key, value
 *
 * with no padding, so it depends only on the level and the items and is the
 * same on every platform.  It is contiguous, from the end of the page header
 * to pd_lower.
 *
 * Item offsets (uint16, native byte order) grow down from the special space,
 * item 0 nearest it, for random access by index.  They are derived from the
 * canonical region and are not hashed.  The special space is
 * PROLLY_BASIC_SPECIAL_SIZE bytes, zeroed at page_init; this format doesn't
 * interpret it.
 *
 * pd_lower, pd_upper and pd_special keep their usual meanings, so data
 * checksums and the buffer manager's page verification work unchanged.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/nodeformat_basic.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "port/pg_bswap.h"

/* Room for a cached hash and flags; see prolly_nodeformat.h. */
#define PROLLY_BASIC_SPECIAL_SIZE	MAXALIGN(PROLLY_HASH_MAX_LEN + 8)

/* level and count */
#define NODE_HEADER_SIZE	(2 * sizeof(uint16))
/* key and value lengths */
#define ITEM_HEADER_SIZE	(2 * sizeof(uint16))

#define CANONICAL_START		SizeOfPageHeaderData

static inline uint16
get_be16(const char *p)
{
	uint16		v;

	memcpy(&v, p, sizeof(v));
	return pg_ntoh16(v);
}

static inline void
put_be16(char *p, uint16 v)
{
	v = pg_hton16(v);
	memcpy(p, &v, sizeof(v));
}

static inline uint16 *
offsets_base(Page page)
{
	/* offset of item i is at index -(i + 1) */
	return (uint16 *) PageGetSpecialPointer(page);
}

static inline uint16
item_offset(Page page, int index)
{
	return offsets_base(page)[-(index + 1)];
}

static void
basic_page_init(Page page, Size page_size, uint16 level)
{
	char	   *hdr;

	PageInit(page, page_size, PROLLY_BASIC_SPECIAL_SIZE);
	hdr = (char *) page + CANONICAL_START;
	put_be16(hdr, level);
	put_be16(hdr + sizeof(uint16), 0);
	((PageHeader) page)->pd_lower = CANONICAL_START + NODE_HEADER_SIZE;
}

static uint16
basic_page_level(Page page)
{
	return get_be16((char *) page + CANONICAL_START);
}

static int
basic_item_count(Page page)
{
	return get_be16((char *) page + CANONICAL_START + sizeof(uint16));
}

static void
basic_item_get(Page page, int index, ProllyKey *key, ProllyValue *value)
{
	const char *item;
	uint16		klen;
	uint16		vlen;

	Assert(index >= 0 && index < basic_item_count(page));
	item = (const char *) page + item_offset(page, index);
	klen = get_be16(item);
	vlen = get_be16(item + sizeof(uint16));
	key->data = (const uint8 *) item + ITEM_HEADER_SIZE;
	key->len = klen;
	value->data = (const uint8 *) item + ITEM_HEADER_SIZE + klen;
	value->len = vlen;
}

static Size
basic_item_space(const ProllyKey *key, const ProllyValue *value)
{
	return ITEM_HEADER_SIZE + key->len + value->len + sizeof(uint16);
}

static Size
basic_free_space(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	return phdr->pd_upper - phdr->pd_lower;
}

static bool
basic_item_append(Page page, const ProllyKey *key, const ProllyValue *value)
{
	PageHeader	phdr = (PageHeader) page;
	char	   *count_ptr = (char *) page + CANONICAL_START + sizeof(uint16);
	int			count = get_be16(count_ptr);
	char	   *item;

	if (key->len > PG_UINT16_MAX || value->len > PG_UINT16_MAX ||
		basic_item_space(key, value) > basic_free_space(page) ||
		count == PG_UINT16_MAX)
		return false;

	item = (char *) page + phdr->pd_lower;
	put_be16(item, (uint16) key->len);
	put_be16(item + sizeof(uint16), (uint16) value->len);
	memcpy(item + ITEM_HEADER_SIZE, key->data, key->len);
	memcpy(item + ITEM_HEADER_SIZE + key->len, value->data, value->len);

	offsets_base(page)[-(count + 1)] = phdr->pd_lower;
	phdr->pd_upper -= sizeof(uint16);
	phdr->pd_lower += ITEM_HEADER_SIZE + key->len + value->len;
	put_be16(count_ptr, (uint16) (count + 1));
	return true;
}

static void
basic_canonical_bytes(Page page, const uint8 **data, Size *len)
{
	*data = (const uint8 *) page + CANONICAL_START;
	*len = ((PageHeader) page)->pd_lower - CANONICAL_START;
}

static char *
basic_special_space(Page page, Size *len)
{
	*len = PageGetSpecialSize(page);
	return PageGetSpecialPointer(page);
}

static bool
basic_page_check(Page page, char **detail)
{
	PageHeader	phdr = (PageHeader) page;
	Size		page_size = PageGetPageSize(page);
	int			count;
	Size		off;

	if (phdr->pd_special > page_size ||
		page_size - phdr->pd_special != PROLLY_BASIC_SPECIAL_SIZE)
	{
		*detail = psprintf("special space at %u is not %zu bytes",
						   phdr->pd_special, (Size) PROLLY_BASIC_SPECIAL_SIZE);
		return false;
	}
	if (phdr->pd_lower < CANONICAL_START + NODE_HEADER_SIZE ||
		phdr->pd_lower > phdr->pd_upper || phdr->pd_upper > phdr->pd_special)
	{
		*detail = psprintf("bad bounds: lower %u, upper %u, special %u",
						   phdr->pd_lower, phdr->pd_upper, phdr->pd_special);
		return false;
	}

	count = basic_item_count(page);
	if (phdr->pd_special - phdr->pd_upper != count * sizeof(uint16))
	{
		*detail = psprintf("item count %d does not match %u bytes of offsets",
						   count, phdr->pd_special - phdr->pd_upper);
		return false;
	}

	/* Walk the items in order; each must start where its offset says. */
	off = CANONICAL_START + NODE_HEADER_SIZE;
	for (int i = 0; i < count; i++)
	{
		const char *item = (const char *) page + off;

		if (item_offset(page, i) != off)
		{
			*detail = psprintf("item %d is at %zu, but its offset says %u",
							   i, off, item_offset(page, i));
			return false;
		}
		if (off + ITEM_HEADER_SIZE > phdr->pd_lower)
		{
			*detail = psprintf("item %d header runs past the canonical region", i);
			return false;
		}
		off += ITEM_HEADER_SIZE + get_be16(item) + get_be16(item + sizeof(uint16));
		if (off > phdr->pd_lower)
		{
			*detail = psprintf("item %d runs past the canonical region", i);
			return false;
		}
	}
	if (off != phdr->pd_lower)
	{
		*detail = psprintf("%zu bytes after the last item", phdr->pd_lower - off);
		return false;
	}
	return true;
}

const ProllyNodeFormatRoutine prolly_nodeformat_basic = {
	.info = {
		.iface = PROLLY_IFACE_NODE_FORMAT,
		.impl_id = 1,
		.format_version = 1,
		.name = "basic",
		.description = "items packed in key order after the page header; "
		"big-endian lengths; unhashed offsets and special space",
	},
	.page_init = basic_page_init,
	.page_level = basic_page_level,
	.item_count = basic_item_count,
	.item_get = basic_item_get,
	.item_append = basic_item_append,
	.item_space = basic_item_space,
	.free_space = basic_free_space,
	.canonical_bytes = basic_canonical_bytes,
	.special_space = basic_special_space,
	.page_check = basic_page_check,
};
