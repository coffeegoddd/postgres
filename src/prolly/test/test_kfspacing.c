/*
 * test_kfspacing.c
 *	  Keyframe spacing: even spacing keeps a keyframe every delta and lets
 *	  go of none; exponential spacing keeps them dense near the head and
 *	  sparse with age, every commit d back within about d (r - 1) / (2 r)
 *	  of one, with keyframes held growing with the logarithm of history.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kfspacing.h"

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "\n%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			exit(1); \
		} \
	} while (0)

#define MAXK 100000

typedef struct Held
{
	uint64_t	at[MAXK];
	size_t		n;
} Held;

/* Commits of 'per' bytes of change each, n of them, under spacing 's'. */
static void
run(KfSpacing *s, uint64_t per, size_t commits, Held *h, uint64_t *head)
{
	static bool drop[MAXK];

	h->n = 0;
	*head = 0;
	for (size_t c = 0; c < commits; c++)
	{
		*head += per;
		if (s->ops->take(s, *head, h->n ? h->at[h->n - 1] : 0))
		{
			CHECK(h->n < MAXK);
			h->at[h->n++] = *head;
		}
		if (s->ops->thin(s, *head, h->at, h->n, drop))
		{
			size_t		k = 0;

			for (size_t i = 0; i < h->n; i++)
				if (!drop[i])
					h->at[k++] = h->at[i];
			h->n = k;
		}
		CHECK(h->n > 0 && h->at[h->n - 1] <= *head);
	}
}

/* The distance from position 'p' to the nearest held keyframe. */
static uint64_t
nearest(const Held *h, uint64_t p)
{
	uint64_t	best = UINT64_MAX;

	for (size_t i = 0; i < h->n; i++)
	{
		uint64_t	d = h->at[i] > p ? h->at[i] - p : p - h->at[i];

		if (d < best)
			best = d;
	}
	return best;
}

static void
test_even(void)
{
	KfSpacing  *s = kf_even_spacing(1000);
	static Held h;
	uint64_t	head;

	run(s, 100, 10000, &h, &head);
	/* one every 1000 bytes of a million, none let go of */
	CHECK(h.n == 1000);
	for (uint64_t p = 100; p <= head; p += 37)
		CHECK(nearest(&h, p) <= 1000);
	s->ops->destroy(s);
	printf("  even: %zu keyframes over %llu bytes\n", h.n, (unsigned long long) head);
}

static void
test_exponential(double r)
{
	uint64_t	delta0 = 1000;
	KfSpacing  *s = kf_exponential_spacing(delta0, r);
	static Held h;
	uint64_t	head;
	size_t		worst_over = 0;

	run(s, 100, 100000, &h, &head);		/* ten million bytes of change */

	/*
	 * Every commit d back is within half the spacing kept at its distance,
	 * d (r - 1) / (2 r), plus one delta0 for the keyframes' own
	 * granularity.
	 */
	for (uint64_t p = 100; p <= head; p += 997)
	{
		uint64_t	d = head - p;
		uint64_t	bound = kf_exponential_gap(delta0, r, d) / 2 + delta0;

		if (nearest(&h, p) > bound)
			worst_over++;
	}
	CHECK(worst_over == 0);

	/*
	 * Held keyframes grow with the logarithm of history: about
	 * log(H / delta0) / log(r / (r - 1)) of them, far fewer than an even
	 * spacing's H / delta0.
	 */
	{
		double		x = (double) head / (double) delta0;
		double		lg = 0,
					base = r / (r - 1);

		for (double y = 1; y < x; y *= base)
			lg += 1;
		CHECK(h.n <= 2 * (size_t) lg + 4);
		CHECK(h.n * 20 < (size_t) x);
		printf("  exponential r=%.0f: %zu keyframes over %llu bytes (even: %zu)\n",
			   r, h.n, (unsigned long long) head, (size_t) x);
	}

	/* the newest is always kept, and near the head they are delta0 apart */
	CHECK(head - h.at[h.n - 1] < delta0);
	CHECK(h.at[h.n - 1] - h.at[h.n - 2] <= 2 * delta0);
	s->ops->destroy(s);
}

/* A keyframe once let go of never comes back: the gaps only grow. */
static void
test_exponential_monotone(void)
{
	KfSpacing  *s = kf_exponential_spacing(500, 2);
	static Held a,
				b;
	uint64_t	ha,
				hb;

	run(s, 100, 20000, &a, &ha);
	run(s, 100, 40000, &b, &hb);
	/* every keyframe held at the end was held midway, if taken by then */
	for (size_t i = 0; i < b.n; i++)
	{
		bool		was = false;

		if (b.at[i] > ha)
			continue;
		for (size_t j = 0; j < a.n; j++)
			if (a.at[j] == b.at[i])
				was = true;
		CHECK(was);
	}
	s->ops->destroy(s);
}

int
main(void)
{
	test_even();
	test_exponential(2);
	test_exponential(4);
	test_exponential_monotone();
	printf("test_kfspacing: ok\n");
	return 0;
}
