/*
 * storebench.c
 *	  dirstore's side of the history store comparison (the others are rows
 *	  of a table and pages of our own layout, in bench/store_compare.sh):
 *	  N random chunks of about 4 KB, committed (flushed: one fdatasync) a
 *	  batch at a time; every chunk read back in random order; a random half
 *	  discarded and the space taken back by compaction.  Prints one line per
 *	  phase: seconds, bytes on disk.
 *
 *	  Chunks are reached as history reaches them, through trees: each batch
 *	  is a node over its chunks, and only that node is a root, so the
 *	  store's root set stays small.  The discard lets go of a random half of
 *	  the batches.
 *
 *	  storebench DIR [N] [BATCH]
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../test/test_util.h"
#include "chunkfmt.h"

/* A leaf node of one key and a random value: a chunk of about len bytes. */
static void
make_chunk(Buf *b, uint64_t n, size_t len)
{
	uint8_t		key[8];

	b->len = 0;
	node_header_write(b, &prolly_default_policy, 0, 0, 1);
	put_u64(key, n);
	buf_append_uvarint(b, sizeof(key));
	buf_append(b, key, sizeof(key));
	len -= b->len + sizeof(key) + 4;
	buf_append_uvarint(b, len);
	for (size_t i = 0; i < len; i += 8)
	{
		uint8_t		r[8];

		put_u64(r, rnd());
		buf_append(b, r, len - i < 8 ? len - i : 8);
	}
}

/* A level-1 node over the chunks h[0..n): each item a key and (hash, count). */
static void
make_node(Buf *b, const PHash *h, int n, uint64_t first)
{
	b->len = 0;
	node_header_write(b, &prolly_default_policy, 1, 0, (uint32_t) n);
	for (int i = 0; i < n; i++)
	{
		uint8_t		key[8];

		put_u64(key, first + (uint64_t) i);
		buf_append_uvarint(b, sizeof(key));
		buf_append(b, key, sizeof(key));
		buf_append_uvarint(b, PHASH_LEN + 1);
		buf_append(b, h[i].b, PHASH_LEN);
		buf_append_uvarint(b, 1);
	}
}

int
main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : NULL;
	int			n = argc > 2 ? atoi(argv[2]) : 20000,
				batch = argc > 3 ? atoi(argv[3]) : 100;
	char		err[256];
	ChunkStore *cs;
	PHash	   *h,
			   *roots;
	int			nroots = 0;
	Buf			b = {0};
	double		t;
	uint64_t	sum = 0;
	DirCompactStats st;

	if (dir == NULL)
	{
		fprintf(stderr, "storebench DIR [N] [BATCH]\n");
		return 2;
	}
	mkdir(dir, 0700);
	cs = dirstore_open(dir, err, sizeof(err));
	if (cs == NULL)
	{
		fprintf(stderr, "storebench: %s\n", err);
		return 1;
	}
	rng_seed(42);
	h = px_malloc(sizeof(PHash) * (size_t) n);
	roots = px_malloc(sizeof(PHash) * (size_t) (n / batch + 1));

	t = now_sec();
	for (int i = 0; i < n; i += batch)
	{
		int			m = n - i < batch ? n - i : batch;

		for (int j = 0; j < m; j++)
		{
			make_chunk(&b, (uint64_t) (i + j), 4096);
			CHECK(cs_put(cs, b.data, b.len, &h[i + j], NULL) == CS_OK);
			cs_pin(cs, &h[i + j]);
		}
		make_node(&b, &h[i], m, (uint64_t) i);
		CHECK(cs_put(cs, b.data, b.len, &roots[nroots], NULL) == CS_OK);
		cs_ref_root(cs, &roots[nroots++]);
		for (int j = 0; j < m; j++)
			cs_unpin(cs, &h[i + j]);
		CHECK_OK(dirstore_flush(cs));
	}
	printf("write    %7.2f s   disk %6.1f MB\n", now_sec() - t, dirstore_disk_bytes(cs) / 1048576.0);

	/* every chunk, in random order */
	for (int i = n - 1; i > 0; i--)
	{
		int			j = (int) rnd_below((uint64_t) i + 1);
		PHash		x = h[i];

		h[i] = h[j];
		h[j] = x;
	}
	t = now_sec();
	for (int i = 0; i < n; i++)
	{
		size_t		len;

		const uint8_t *p = cs_get(cs, &h[i], &len);

		CHECK(p != NULL);
		/* every byte, as the other stores copy every byte */
		for (size_t j = 0; j < len; j++)
			sum += p[j];
	}
	printf("read     %7.2f s   (checksum %" PRIu64 ")\n", now_sec() - t, sum);

	/* reopened: the store's own cache empty, the operating system's not */
	t = now_sec();
	cs_destroy(cs);
	cs = dirstore_open(dir, err, sizeof(err));
	CHECK(cs != NULL);
	printf("reopen   %7.2f s\n", now_sec() - t);
	sum = 0;
	t = now_sec();
	for (int i = 0; i < n; i++)
	{
		size_t		len;

		const uint8_t *p = cs_get(cs, &h[i], &len);

		CHECK(p != NULL);
		/* every byte, as the other stores copy every byte */
		for (size_t j = 0; j < len; j++)
			sum += p[j];
	}
	printf("read cold%7.2f s   (checksum %" PRIu64 ")\n", now_sec() - t, sum);

	/* a random half of the batches let go, then the space taken back */
	t = now_sec();
	for (int i = 0; i < nroots; i++)
		if (rnd_below(2) == 0)
			cs_unref_root(cs, &roots[i]);
	cs_collect_orphans(cs);
	CHECK_OK(dirstore_flush(cs));
	CHECK_OK(dirstore_compact(cs, 0.0, &st));
	printf("discard  %7.2f s   disk %6.1f MB\n", now_sec() - t, dirstore_disk_bytes(cs) / 1048576.0);

	free(h);
	free(roots);
	free(b.data);
	cs_destroy(cs);
	return 0;
}
