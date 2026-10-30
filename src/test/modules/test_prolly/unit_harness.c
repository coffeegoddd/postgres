/*-------------------------------------------------------------------------
 *
 * unit_harness.c
 *	  Unit tests of the test harness itself: its checks fail when they
 *	  should, and its helpers give the same results everywhere.
 *
 * A check that never fails would let every test pass, so each kind of
 * check is shown failing, with the message a reader of a failure gets.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_prolly/unit_harness.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "test_prolly.h"
#include "utils/memutils.h"

/*
 * Run a block that must raise an error whose message is want.  The error is
 * caught and cleared here; the block raises it before doing anything that
 * would need a subtransaction to undo.
 */
#define EXPECT_FAILURE(want, block) \
	do { \
		MemoryContext expect_cxt_ = CurrentMemoryContext; \
		char	   *expect_msg_ = NULL; \
		PG_TRY(); \
		{ \
			block; \
		} \
		PG_CATCH(); \
		{ \
			ErrorData  *expect_edata_; \
			MemoryContextSwitchTo(expect_cxt_); \
			expect_edata_ = CopyErrorData(); \
			FlushErrorState(); \
			expect_msg_ = expect_edata_->message; \
		} \
		PG_END_TRY(); \
		if (expect_msg_ == NULL) \
			elog(ERROR, "expected a failure: %s", #block); \
		CHECK_STR_EQ(expect_msg_, want); \
	} while (0)

static void
checks_pass(void)
{
	CHECK(1 + 1 == 2);
	CHECK_EQ(3, 3);
	CHECK_NE(3, 4);
	CHECK_LE(3, 3);
	CHECK_LT(3, 4);
	CHECK_STR_EQ("abc", "abc");
}

static void
check_fails(void)
{
	EXPECT_FAILURE("CHECK failed: 1 + 1 == 3", CHECK(1 + 1 == 3));
}

static void
comparisons_fail_with_values(void)
{
	uint64		big = PG_UINT64_MAX;

	EXPECT_FAILURE("CHECK failed: 2 == 3 (2 vs 3)", CHECK_EQ(2, 3));
	EXPECT_FAILURE("CHECK failed: 3 != 3 (3 vs 3)", CHECK_NE(3, 3));
	EXPECT_FAILURE("CHECK failed: 4 <= 3 (4 vs 3)", CHECK_LE(4, 3));
	EXPECT_FAILURE("CHECK failed: 3 < 3 (3 vs 3)", CHECK_LT(3, 3));
	/* values are compared, and shown, as 64-bit unsigned */
	EXPECT_FAILURE("CHECK failed: big == 0 (18446744073709551615 vs 0)",
				   CHECK_EQ(big, 0));
	EXPECT_FAILURE("CHECK failed: \"a\" == \"b\" (\"a\" vs \"b\")",
				   CHECK_STR_EQ("a", "b"));
}

/*
 * The generator must give the sequence the engine's earlier tests were
 * written against, so a seed recorded from a failure there replays here.
 */
static void
rng_sequence(void)
{
	ProllyTestRng rng;
	ProllyTestRng again;

	rng_seed(&rng, 1);
	CHECK_EQ(rnd(&rng), UINT64CONST(0x806132229cb46b5d));
	CHECK_EQ(rnd(&rng), UINT64CONST(0x060f1487c439fe10));
	CHECK_EQ(rnd(&rng), UINT64CONST(0xbfff144d8a4f7aec));

	rng_seed(&rng, 2);
	rng_seed(&again, 2);
	for (int i = 0; i < 100; i++)
	{
		uint64		v = rnd_below(&rng, 10);

		CHECK_LT(v, 10);
		CHECK_EQ(v, rnd_below(&again, 10));
	}
}

static void
u64_keys_sort_as_bytes(void)
{
	uint8		a[8];
	uint8		b[8];

	put_u64(a, UINT64CONST(0x0102030405060708));
	CHECK_EQ(a[0], 0x01);
	CHECK_EQ(a[7], 0x08);
	CHECK_EQ(get_u64(a), UINT64CONST(0x0102030405060708));

	put_u64(a, 255);
	put_u64(b, 256);
	CHECK(memcmp(a, b, 8) < 0);
}

static const ProllyUnitTest tests[] = {
	{"checks_pass", checks_pass},
	{"check_fails", check_fails},
	{"comparisons_fail_with_values", comparisons_fail_with_values},
	{"rng_sequence", rng_sequence},
	{"u64_keys_sort_as_bytes", u64_keys_sort_as_bytes},
};

const ProllyUnitSuite unit_harness_suite = {"harness", tests, lengthof(tests)};
