/*-------------------------------------------------------------------------
 *
 * util.h
 *	  The engine's helpers: memory, a growing byte buffer, varints and key
 *	  order, on the backend's memory contexts.
 *
 * Every allocation is a palloc in the current memory context (huge ones
 * allowed): an error raised anywhere unwinds them with the context, so an
 * operation that fails leaks nothing past its transaction, or past the
 * context its caller runs it in.  Running out of memory is an error, not a
 * NULL.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/engine/util.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ENGINE_UTIL_H
#define PROLLY_ENGINE_UTIL_H

#include "utils/memutils.h"

static inline void *
px_malloc(size_t n)
{
	return MemoryContextAllocHuge(CurrentMemoryContext, n ? n : 1);
}

static inline void *
px_realloc(void *p, size_t n)
{
	if (p == NULL)
		return px_malloc(n);
	return repalloc_huge(p, n ? n : 1);
}

static inline void
px_free(void *p)
{
	if (p != NULL)
		pfree(p);
}

static inline void *
px_memdup(const void *p, size_t n)
{
	void	   *q = px_malloc(n);

	if (n)
		memcpy(q, p, n);
	return q;
}

/* *v, of *cap elements of esz bytes, grown to hold need of them */
static inline void
px_grow(void **v, size_t *cap, size_t need, size_t esz)
{
	size_t		c;

	if (need <= *cap)
		return;
	c = *cap ? *cap * 2 : 16;
	while (c < need)
		c *= 2;
	*v = px_realloc(*v, c * esz);
	*cap = c;
}

typedef struct Buf
{
	uint8	   *data;
	size_t		len;
	size_t		cap;
} Buf;

#define BUF_INIT {NULL, 0, 0}

static inline void
buf_reserve(Buf *b, size_t extra)
{
	px_grow((void **) &b->data, &b->cap, b->len + extra, 1);
}

static inline void
buf_append(Buf *b, const void *p, size_t n)
{
	buf_reserve(b, n);
	if (n)
		memcpy(b->data + b->len, p, n);
	b->len += n;
}

static inline void
buf_append_byte(Buf *b, uint8 c)
{
	buf_reserve(b, 1);
	b->data[b->len++] = c;
}

static inline void
buf_append_uvarint(Buf *b, uint64 v)
{
	buf_reserve(b, 10);
	while (v >= 0x80)
	{
		b->data[b->len++] = (uint8) (v | 0x80);
		v >>= 7;
	}
	b->data[b->len++] = (uint8) v;
}

static inline void
buf_free(Buf *b)
{
	px_free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

static inline bool
read_uvarint(const uint8 **p, const uint8 *end, uint64 *out)
{
	uint64		v = 0;
	int			shift = 0;

	while (*p < end && shift < 64)
	{
		uint8		c = *(*p)++;

		v |= (uint64) (c & 0x7f) << shift;
		if ((c & 0x80) == 0)
		{
			*out = v;
			return true;
		}
		shift += 7;
	}
	return false;
}

/* Keys order bytewise; a proper prefix sorts first. */
static inline int
key_cmp(const uint8 *a, size_t al, const uint8 *b, size_t bl)
{
	size_t		n = al < bl ? al : bl;
	int			c = n ? memcmp(a, b, n) : 0;

	if (c != 0)
		return c;
	return (al > bl) - (al < bl);
}

static inline bool
bytes_eq(const uint8 *a, size_t al, const uint8 *b, size_t bl)
{
	return al == bl && (al == 0 || memcmp(a, b, al) == 0);
}

#endif							/* PROLLY_ENGINE_UTIL_H */
