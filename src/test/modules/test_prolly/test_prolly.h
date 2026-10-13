/*--------------------------------------------------------------------------
 *
 * test_prolly.h
 *		Conformance case bodies shared between test_prolly's source files.
 *
 * A body receives the implementation under test and raises an error if a
 * check fails (test_prolly.c runs each in a subtransaction).
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/test_prolly.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef TEST_PROLLY_H
#define TEST_PROLLY_H

#include "access/prolly_config.h"

/* foundation_checks.c */
extern void nodeformat_items_round_trip(const ProllyImplInfo *impl);
extern void nodeformat_level_round_trip(const ProllyImplInfo *impl);
extern void nodeformat_space_accounting(const ProllyImplInfo *impl);
extern void nodeformat_canonical_bytes(const ProllyImplInfo *impl);
extern void nodeformat_page_check(const ProllyImplInfo *impl);
extern void chunker_deterministic(const ProllyImplInfo *impl);
extern void chunker_forced_split(const ProllyImplInfo *impl);
extern void chunker_reset_equals_begin(const ProllyImplInfo *impl);
extern void keyenc_order_matches_opclass(const ProllyImplInfo *impl);
extern void keyenc_desc_and_nulls(const ProllyImplInfo *impl);
extern void keyenc_decode_round_trip(const ProllyImplInfo *impl);
extern void valenc_round_trip(const ProllyImplInfo *impl);
extern void valenc_platform_independent(const ProllyImplInfo *impl);

/* storage_checks.c */
extern void nodestore_new_publish_read(const ProllyImplInfo *impl);
extern void nodestore_many_nodes(const ProllyImplInfo *impl);
extern void nodestore_release_frees(const ProllyImplInfo *impl);

#endif							/* TEST_PROLLY_H */
