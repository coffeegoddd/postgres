/*-------------------------------------------------------------------------
 *
 * test_checksum.c
 *	  An example interface, two implementations of it and its conformance
 *	  suite, which show the conformance runner both passing and failing.
 *
 * The interface computes a checksum of a byte string.  "hash_bytes" is a
 * correct implementation.  "drops_last_byte" ignores the string's last
 * byte, the kind of off-by-one a real implementation could have; the suite
 * must report it as failing, or the suite's checks are too weak to catch
 * anything.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_prolly/test_checksum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "prolly/registry.h"
#include "test_prolly.h"

#define TEST_CHECKSUM_IFACE "test_checksum"

typedef struct TestChecksumRoutine
{
	/* the checksum of len bytes at data; data may be NULL when len is 0 */
	uint32		(*checksum) (const uint8 *data, size_t len);
} TestChecksumRoutine;

static uint32
hash_bytes_checksum(const uint8 *data, size_t len)
{
	return hash_bytes(data, (int) len);
}

static uint32
drops_last_byte_checksum(const uint8 *data, size_t len)
{
	return hash_bytes(data, len > 0 ? (int) len - 1 : 0);
}

static const TestChecksumRoutine hash_bytes_routine = {
	.checksum = hash_bytes_checksum,
};

static const TestChecksumRoutine drops_last_byte_routine = {
	.checksum = drops_last_byte_checksum,
};

void
register_test_checksum_implementations(void)
{
	ProllyRegisterImplementation(TEST_CHECKSUM_IFACE, "hash_bytes",
								 &hash_bytes_routine);
	ProllyRegisterImplementation(TEST_CHECKSUM_IFACE, "drops_last_byte",
								 &drops_last_byte_routine);
}

static const TestChecksumRoutine *
routine(const ProllyImplementation *impl)
{
	return (const TestChecksumRoutine *) impl->routine;
}

static void
fill(uint8 *buf, size_t len, uint64 seed)
{
	ProllyTestRng rng;

	rng_seed(&rng, seed);
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8) rnd(&rng);
}

/* The same bytes give the same checksum, wherever they are in memory. */
static void
deterministic(const ProllyImplementation *impl)
{
	uint8		a[64];
	uint8		b[65];

	fill(a, sizeof(a), 1);
	memcpy(b + 1, a, sizeof(a));
	CHECK_EQ(routine(impl)->checksum(a, sizeof(a)),
			 routine(impl)->checksum(b + 1, sizeof(a)));
	CHECK_EQ(routine(impl)->checksum(NULL, 0), routine(impl)->checksum(a, 0));
}

/* Changing any one byte changes the checksum. */
static void
detects_changed_byte(const ProllyImplementation *impl)
{
	uint8		buf[64];
	uint32		before;

	fill(buf, sizeof(buf), 2);
	before = routine(impl)->checksum(buf, sizeof(buf));
	for (size_t i = 0; i < sizeof(buf); i++)
	{
		buf[i] ^= 0x01;
		CHECK_NE(routine(impl)->checksum(buf, sizeof(buf)), before);
		buf[i] ^= 0x01;
	}
}

/* Strings that differ only in length have different checksums. */
static void
detects_changed_length(const ProllyImplementation *impl)
{
	uint8		buf[16] = {0};

	for (size_t len = 1; len < sizeof(buf); len++)
		CHECK_NE(routine(impl)->checksum(buf, len),
				 routine(impl)->checksum(buf, len - 1));
}

static const ProllyConformanceCase cases[] = {
	{"deterministic", deterministic},
	{"detects_changed_byte", detects_changed_byte},
	{"detects_changed_length", detects_changed_length},
};

const ProllyConformanceSuite conformance_test_checksum_suite = {
	TEST_CHECKSUM_IFACE, cases, lengthof(cases)
};
