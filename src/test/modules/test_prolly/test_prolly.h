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
extern void chunker_deterministic(const ProllyImplInfo *impl);
extern void chunker_forced_split(const ProllyImplInfo *impl);
extern void chunkformat_items_round_trip(const ProllyImplInfo *impl);
extern void chunkformat_rejects_other_bytes(const ProllyImplInfo *impl);
extern void tablefile_round_trip(const ProllyImplInfo *impl);
extern void tablefile_rejects_other_bytes(const ProllyImplInfo *impl);
extern void tablefile_shared_prefixes(const ProllyImplInfo *impl);
extern void keyenc_order_matches_opclass(const ProllyImplInfo *impl);
extern void keyenc_desc_and_nulls(const ProllyImplInfo *impl);
extern void cowimage_keeps_needed_images(const ProllyImplInfo *impl);
extern void valenc_round_trip(const ProllyImplInfo *impl);
extern void valenc_long_values(const ProllyImplInfo *impl);
extern void valenc_kept_values(const ProllyImplInfo *impl);
extern void valenc_platform_independent(const ProllyImplInfo *impl);
extern void valenc_independent_of_history(const ProllyImplInfo *impl);
extern void valenc_independent_of_session(const ProllyImplInfo *impl);
extern void valenc_missing_values(const ProllyImplInfo *impl);
extern void valenc_selected_columns(const ProllyImplInfo *impl);
extern void valenc_assembled_rows(const ProllyImplInfo *impl);

/* tree_checks.c */
extern void tree_build_and_get(const ProllyImplInfo *impl);
extern void tree_large_keys(const ProllyImplInfo *impl);
extern void tree_cursor_scans(const ProllyImplInfo *impl);
extern void tree_apply_matches_reference(const ProllyImplInfo *impl);
extern void tree_apply_past_the_end(const ProllyImplInfo *impl);
extern void tree_history_independent(const ProllyImplInfo *impl);
extern void tree_edits_share_nodes(const ProllyImplInfo *impl);
extern void tree_diff_reports_changes(const ProllyImplInfo *impl);
extern void tree_equal_keys_take_new_bytes(const ProllyImplInfo *impl);
extern void tree_counts_nodes(const ProllyImplInfo *impl);
extern void tree_ranks(const ProllyImplInfo *impl);
extern void tree_diff_reads_changed_paths(const ProllyImplInfo *impl);
extern void tree_diff_stops(const ProllyImplInfo *impl);
extern void tree_diff_across_heights(const ProllyImplInfo *impl);
extern void tree_diff_unordered_matches(const ProllyImplInfo *impl);
extern void tree_diff_estimate_bounds(const ProllyImplInfo *impl);
extern void tree_seeks_in_order(const ProllyImplInfo *impl);
extern void tree_apply_reads_paths_once(const ProllyImplInfo *impl);
extern void tree_nodes_are_valid(const ProllyImplInfo *impl);
extern void tree_large_values(const ProllyImplInfo *impl);
extern void tree_held_references(const ProllyImplInfo *impl);
extern void tree_value_splices(const ProllyImplInfo *impl);

/* keying_checks.c */
extern void keying_round_trip(const ProllyImplInfo *impl);
extern void keying_keys_and_content(const ProllyImplInfo *impl);
extern void keying_owns_its_memory(const ProllyImplInfo *impl);
extern void keying_row_shapes(const ProllyImplInfo *impl);
extern void keying_old_shapes(const ProllyImplInfo *impl);
extern void keying_assembled_rows(const ProllyImplInfo *impl);
extern void valenc_decodes_earlier_layouts(const ProllyImplInfo *impl);

#endif							/* TEST_PROLLY_H */
