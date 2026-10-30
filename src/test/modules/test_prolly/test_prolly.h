/*-------------------------------------------------------------------------
 *
 * test_prolly.h
 *	  Unit tests and conformance suites for the engine's code in the server.
 *
 * Two kinds of test run inside a backend, so that they can call any code
 * compiled into the server:
 *
 * A unit test is a function of no arguments that checks one behavior of one
 * piece of code.  Unit tests are grouped into suites, one per source file
 * of tests.
 *
 * A conformance case is a function that checks one part of an interface's
 * contract against one implementation.  The runner calls every case of an
 * interface once for each implementation registered under that interface,
 * so a new implementation is checked by the same suite as the old ones the
 * moment it registers.
 *
 * Both report a failure by raising an error, normally through the CHECK
 * macros below; the runner gives each call its own subtransaction and
 * memory context, so a failure is reported and the next test still runs.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/test_prolly.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TEST_PROLLY_H
#define TEST_PROLLY_H

#include "prolly/registry.h"

typedef struct ProllyUnitTest
{
	const char *name;
	void		(*fn) (void);
} ProllyUnitTest;

typedef struct ProllyUnitSuite
{
	const char *name;
	const ProllyUnitTest *tests;
	int			ntests;
} ProllyUnitSuite;

typedef struct ProllyConformanceCase
{
	const char *name;
	void		(*fn) (const ProllyImplementation *impl);
} ProllyConformanceCase;

typedef struct ProllyConformanceSuite
{
	const char *iface;			/* the interface every case checks */
	const ProllyConformanceCase *cases;
	int			ncases;
} ProllyConformanceSuite;

/* The suites, each defined in its own source file. */
extern const ProllyUnitSuite unit_harness_suite;
extern const ProllyUnitSuite unit_bufmgr_suite;
extern const ProllyUnitSuite unit_registry_suite;
extern const ProllyUnitSuite unit_smgr_suite;
extern const ProllyConformanceSuite conformance_test_checksum_suite;

/* Registration of the example interface's implementations. */
extern void register_test_checksum_implementations(void);

/*
 * Assertions.  A failed check raises an error naming the condition, and the
 * values for the comparisons; the error's location is the check's.
 */
#define CHECK(cond) \
	do { \
		if (!(cond)) \
			ereport(ERROR, \
					(errcode(ERRCODE_INTERNAL_ERROR), \
					 errmsg_internal("CHECK failed: %s", #cond))); \
	} while (0)

#define CHECK_CMP(a, op, b) \
	do { \
		uint64		check_a_ = (a), \
					check_b_ = (b); \
		if (!(check_a_ op check_b_)) \
			ereport(ERROR, \
					(errcode(ERRCODE_INTERNAL_ERROR), \
					 errmsg_internal("CHECK failed: %s %s %s (" UINT64_FORMAT " vs " UINT64_FORMAT ")", \
									 #a, #op, #b, check_a_, check_b_))); \
	} while (0)

#define CHECK_EQ(a, b) CHECK_CMP(a, ==, b)
#define CHECK_NE(a, b) CHECK_CMP(a, !=, b)
#define CHECK_LE(a, b) CHECK_CMP(a, <=, b)
#define CHECK_LT(a, b) CHECK_CMP(a, <, b)

#define CHECK_STR_EQ(a, b) \
	do { \
		const char *check_a_ = (a), \
				   *check_b_ = (b); \
		if (strcmp(check_a_, check_b_) != 0) \
			ereport(ERROR, \
					(errcode(ERRCODE_INTERNAL_ERROR), \
					 errmsg_internal("CHECK failed: %s == %s (\"%s\" vs \"%s\")", \
									 #a, #b, check_a_, check_b_))); \
	} while (0)

/*
 * Deterministic randomness: an xorshift generator, so a test's inputs are
 * the same on every platform and every run, and a failure can be replayed
 * from its seed.  Each test keeps its own state, so tests do not disturb
 * each other's sequences.
 */
typedef struct ProllyTestRng
{
	uint64		state;
} ProllyTestRng;

static inline void
rng_seed(ProllyTestRng *rng, uint64 seed)
{
	rng->state = seed * UINT64CONST(0x9e3779b97f4a7c15) + 1;
}

static inline uint64
rnd(ProllyTestRng *rng)
{
	uint64		x = rng->state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng->state = x;
	return x * UINT64CONST(0x2545f4914f6cdd1d);
}

static inline uint64
rnd_below(ProllyTestRng *rng, uint64 n)
{
	return rnd(rng) % n;
}

/*
 * Big-endian 64-bit keys, whose byte order is their numeric order, for
 * tests of code that compares keys as bytes.
 */
static inline void
put_u64(uint8 *out, uint64 v)
{
	for (int i = 7; i >= 0; i--, v >>= 8)
		out[i] = (uint8) v;
}

static inline uint64
get_u64(const uint8 *p)
{
	uint64		v = 0;

	for (int i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}

#endif							/* TEST_PROLLY_H */
