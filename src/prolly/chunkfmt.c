/*
 * chunkfmt.c
 *	  Chunk header parsing and reference decoding.
 */
#include "chunkfmt.h"

bool
policy_valid(const ChunkPolicy *p)
{
	return p->min_bytes < p->target_bytes && p->target_bytes <= p->max_bytes;
}

bool
node_header_parse(const uint8_t *data, size_t len, NodeHeader *h)
{
	const uint8_t *p = data;
	const uint8_t *end = data + len;
	uint64_t	v[3],
				count;

	if (len < 2 || p[0] != CHUNK_TAG_NODE || p[1] != CHUNK_FORMAT_VERSION)
		return false;
	p += 2;
	for (int i = 0; i < 3; i++)
		if (!read_uvarint(&p, end, &v[i]) || v[i] > UINT32_MAX)
			return false;
	h->policy.min_bytes = (uint32_t) v[0];
	h->policy.target_bytes = (uint32_t) v[1];
	h->policy.max_bytes = (uint32_t) v[2];
	if (!policy_valid(&h->policy) || end - p < 2)
		return false;
	h->level = p[0];
	h->vrefs = p[1];
	p += 2;
	if (!read_uvarint(&p, end, &count) || count > len)
		return false;
	h->count = (uint32_t) count;
	h->items = p;
	h->end = end;
	return true;
}

void
node_header_write(Buf *b, const ChunkPolicy *p, int level, uint8_t vrefs,
				  uint32_t count)
{
	buf_append_byte(b, CHUNK_TAG_NODE);
	buf_append_byte(b, CHUNK_FORMAT_VERSION);
	buf_append_uvarint(b, p->min_bytes);
	buf_append_uvarint(b, p->target_bytes);
	buf_append_uvarint(b, p->max_bytes);
	buf_append_byte(b, (uint8_t) level);
	buf_append_byte(b, vrefs);
	buf_append_uvarint(b, count);
}

static bool
read_hash(const uint8_t **p, const uint8_t *end, PHash *h)
{
	if ((size_t) (end - *p) < PHASH_LEN)
		return false;
	memcpy(h->b, *p, PHASH_LEN);
	*p += PHASH_LEN;
	return true;
}

static bool
node_refs(const uint8_t *data, size_t len, ChunkRefFn fn, void *arg)
{
	NodeHeader	h;
	const uint8_t *p;

	if (!node_header_parse(data, len, &h))
		return false;
	p = h.items;
	for (uint32_t i = 0; i < h.count; i++)
	{
		uint64_t	kl,
					vl,
					total;
		const uint8_t *v,
				   *vend;
		PHash		ref;

		if (!read_uvarint(&p, h.end, &kl) || kl > (uint64_t) (h.end - p))
			return false;
		p += kl;
		if (!read_uvarint(&p, h.end, &vl) || vl > (uint64_t) (h.end - p))
			return false;
		v = p;
		vend = p + vl;
		p = vend;
		if (h.level > 0)
		{
			if (!read_hash(&v, vend, &ref) || !read_uvarint(&v, vend, &total) ||
				v != vend)
				return false;
			fn(arg, &ref, true);
		}
		else
			for (int r = 0; r < h.vrefs; r++)
			{
				if (!read_hash(&v, vend, &ref))
					return false;
				fn(arg, &ref, true);
			}
	}
	return p == h.end;
}

static bool
commit_refs(const uint8_t *data, size_t len, ChunkRefFn fn, void *arg)
{
	const uint8_t *p = data + 2;
	const uint8_t *end = data + len;
	PHash		h;
	uint64_t	gen,
				np;

	if (!read_hash(&p, end, &h))
		return false;
	fn(arg, &h, true);
	if (!read_uvarint(&p, end, &gen) || !read_uvarint(&p, end, &gen) ||
		!read_uvarint(&p, end, &gen) || !read_uvarint(&p, end, &np) || np > 64)
		return false;
	for (uint64_t i = 0; i < np; i++)
	{
		if (!read_hash(&p, end, &h) || !read_uvarint(&p, end, &gen) ||
			!read_uvarint(&p, end, &gen) || !read_uvarint(&p, end, &gen))
			return false;
		fn(arg, &h, false);
	}
	return true;
}

static bool
delta_refs(const uint8_t *data, size_t len, ChunkRefFn fn, void *arg)
{
	const uint8_t *p = data + 2;
	const uint8_t *end = data + len;
	PHash		base,
				target;

	if (!read_hash(&p, end, &base) || !read_hash(&p, end, &target))
		return false;
	fn(arg, &base, false);
	fn(arg, &target, false);
	return true;
}

static bool
manifest_refs(const uint8_t *data, size_t len, ChunkRefFn fn, void *arg)
{
	const uint8_t *p = data + 2;
	const uint8_t *end = data + len;
	uint64_t	nl,
				n,
				nb,
				gen;
	PHash		h;

	if (!read_uvarint(&p, end, &nl) || nl > (uint64_t) (end - p))
		return false;
	p += nl;
	if (!read_uvarint(&p, end, &n) || n > len)
		return false;
	for (uint64_t i = 0; i < n; i++)
	{
		if (!read_hash(&p, end, &h) || !read_uvarint(&p, end, &gen))
			return false;
		fn(arg, &h, true);
	}
	if (!read_uvarint(&p, end, &nb) || nb > len)
		return false;
	for (uint64_t i = 0; i < nb; i++)
	{
		if (!read_hash(&p, end, &h))
			return false;
		fn(arg, &h, false);
	}
	return p == end;
}

static bool
image_refs(const uint8_t *data, size_t len, ChunkRefFn fn, void *arg)
{
	const uint8_t *p = data + 2;
	const uint8_t *end = data + len;
	uint64_t	v,
				nxip;
	PHash		h;

	for (int i = 0; i < 3; i++)
		if (!read_uvarint(&p, end, &v))
			return false;
	if (end - p < 8)
		return false;
	p += 8;
	if (!read_hash(&p, end, &h))
		return false;
	fn(arg, &h, false);
	if (p >= end)
		return false;
	if (*p++)
	{
		if (!read_hash(&p, end, &h))
			return false;
		fn(arg, &h, false);
	}
	for (int i = 0; i < 3; i++)	/* lsn, xmin, xmax */
		if (!read_uvarint(&p, end, &v))
			return false;
	if (!read_uvarint(&p, end, &nxip) || nxip > len)
		return false;
	for (uint64_t i = 0; i < nxip; i++)
		if (!read_uvarint(&p, end, &v))
			return false;
	for (int i = 0; i < 2; i++)
	{
		if (!read_hash(&p, end, &h))
			return false;
		fn(arg, &h, true);
	}
	return p == end;
}

bool
chunk_refs(const uint8_t *data, size_t len, ChunkRefFn fn, void *arg)
{
	if (len < 2 || data[1] != CHUNK_FORMAT_VERSION)
		return false;
	switch (data[0])
	{
		case CHUNK_TAG_NODE:
			return node_refs(data, len, fn, arg);
		case CHUNK_TAG_SCHEMA:
			return true;
		case CHUNK_TAG_COMMIT:
			return commit_refs(data, len, fn, arg);
		case CHUNK_TAG_DELTA:
			return delta_refs(data, len, fn, arg);
		case CHUNK_TAG_MANIFEST:
			return manifest_refs(data, len, fn, arg);
		case CHUNK_TAG_PAGE:
			return true;
		case CHUNK_TAG_IMAGE:
			return image_refs(data, len, fn, arg);
		case CHUNK_TAG_REPO:
			return true;
		default:
			return false;
	}
}
