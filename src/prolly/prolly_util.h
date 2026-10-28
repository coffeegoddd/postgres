/*
 * prolly_util.h
 *	  Small helpers shared by the prolly prototype: checked allocation,
 *	  growable byte buffers, varints and key ordering.
 */
#ifndef PROLLY_UTIL_H
#define PROLLY_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void *
px_malloc(size_t n)
{
	void	   *p = malloc(n ? n : 1);

	if (p == NULL)
	{
		fprintf(stderr, "prolly: out of memory\n");
		abort();
	}
	return p;
}

static inline void *
px_realloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (p == NULL)
	{
		fprintf(stderr, "prolly: out of memory\n");
		abort();
	}
	return p;
}

static inline void *
px_memdup(const void *p, size_t n)
{
	void	   *q = px_malloc(n);

	if (n)
		memcpy(q, p, n);
	return q;
}

/* Grow *v (element size esz, capacity *cap) to hold at least need elements. */
static inline void
px_grow(void **v, size_t *cap, size_t need, size_t esz)
{
	if (need <= *cap)
		return;
	size_t		c = *cap ? *cap * 2 : 16;

	while (c < need)
		c *= 2;
	*v = px_realloc(*v, c * esz);
	*cap = c;
}

typedef struct Buf
{
	uint8_t    *data;
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
buf_append_byte(Buf *b, uint8_t c)
{
	buf_reserve(b, 1);
	b->data[b->len++] = c;
}

static inline void
buf_append_uvarint(Buf *b, uint64_t v)
{
	buf_reserve(b, 10);
	while (v >= 0x80)
	{
		b->data[b->len++] = (uint8_t) (v | 0x80);
		v >>= 7;
	}
	b->data[b->len++] = (uint8_t) v;
}

static inline void
buf_free(Buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

static inline bool
read_uvarint(const uint8_t **p, const uint8_t *end, uint64_t *out)
{
	uint64_t	v = 0;
	int			shift = 0;

	while (*p < end && shift < 64)
	{
		uint8_t		c = *(*p)++;

		v |= (uint64_t) (c & 0x7f) << shift;
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
key_cmp(const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
	size_t		n = al < bl ? al : bl;
	int			c = n ? memcmp(a, b, n) : 0;

	if (c != 0)
		return c;
	return (al > bl) - (al < bl);
}

static inline bool
bytes_eq(const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
	return al == bl && (al == 0 || memcmp(a, b, al) == 0);
}

#endif							/* PROLLY_UTIL_H */
