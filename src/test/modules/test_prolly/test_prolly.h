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
extern void valenc_independent_of_history(const ProllyImplInfo *impl);
extern void valenc_independent_of_session(const ProllyImplInfo *impl);
extern void valenc_missing_values(const ProllyImplInfo *impl);

/* storage_checks.c */
extern void nodestore_new_publish_read(const ProllyImplInfo *impl);
extern void nodestore_many_nodes(const ProllyImplInfo *impl);
extern void nodestore_release_frees(const ProllyImplInfo *impl);

/* tree_checks.c */
extern void tree_build_and_get(const ProllyImplInfo *impl);
extern void tree_cursor_scans(const ProllyImplInfo *impl);
extern void tree_apply_matches_reference(const ProllyImplInfo *impl);
extern void tree_apply_past_the_end(const ProllyImplInfo *impl);
extern void tree_history_independent(const ProllyImplInfo *impl);
extern void tree_edits_share_nodes(const ProllyImplInfo *impl);
extern void tree_diff_reports_changes(const ProllyImplInfo *impl);
extern void tree_nodes_are_valid(const ProllyImplInfo *impl);

/* rootlayout_checks.c */
extern void rootlayout_empty(const ProllyImplInfo *impl);
extern void rootlayout_apply_matches_reference(const ProllyImplInfo *impl);
extern void rootlayout_history_independent(const ProllyImplInfo *impl);
extern void rootlayout_subroots_independent(const ProllyImplInfo *impl);
extern void rootlayout_diff_reports_changes(const ProllyImplInfo *impl);
extern void rootlayout_apply_rejects_invalid_refs(const ProllyImplInfo *impl);
extern void rootlayout_walk_reaches_trees(const ProllyImplInfo *impl);

#endif							/* TEST_PROLLY_H */
