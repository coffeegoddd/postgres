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

/* tree checks build their trees as chunks in the chunk store */
extern bool tree_checks_in_chunks;

/* foundation_checks.c */
extern void nodeformat_items_round_trip(const ProllyImplInfo *impl);
extern void nodeformat_level_round_trip(const ProllyImplInfo *impl);
extern void nodeformat_space_accounting(const ProllyImplInfo *impl);
extern void nodeformat_canonical_bytes(const ProllyImplInfo *impl);
extern void nodeformat_page_check(const ProllyImplInfo *impl);
extern void chunker_deterministic(const ProllyImplInfo *impl);
extern void chunker_forced_split(const ProllyImplInfo *impl);
extern void chunker_reset_equals_begin(const ProllyImplInfo *impl);
extern void chunkformat_items_round_trip(const ProllyImplInfo *impl);
extern void chunkformat_rejects_other_bytes(const ProllyImplInfo *impl);
extern void keyenc_order_matches_opclass(const ProllyImplInfo *impl);
extern void keyenc_desc_and_nulls(const ProllyImplInfo *impl);
extern void keyenc_decode_round_trip(const ProllyImplInfo *impl);
extern void keyenc_prefix_lengths(const ProllyImplInfo *impl);
extern void keyenc_equal_bytes(const ProllyImplInfo *impl);
extern void ixkeying_entries(const ProllyImplInfo *impl);
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
extern void tree_large_keys(const ProllyImplInfo *impl);
extern void tree_cursor_scans(const ProllyImplInfo *impl);
extern void tree_apply_matches_reference(const ProllyImplInfo *impl);
extern void tree_apply_past_the_end(const ProllyImplInfo *impl);
extern void tree_history_independent(const ProllyImplInfo *impl);
extern void tree_edits_share_nodes(const ProllyImplInfo *impl);
extern void tree_diff_reports_changes(const ProllyImplInfo *impl);
extern void tree_equal_keys_take_new_bytes(const ProllyImplInfo *impl);
extern void tree_counts_nodes(const ProllyImplInfo *impl);
extern void tree_diff_reads_changed_paths(const ProllyImplInfo *impl);
extern void tree_diff_across_heights(const ProllyImplInfo *impl);
extern void tree_seeks_in_order(const ProllyImplInfo *impl);
extern void tree_nodes_are_valid(const ProllyImplInfo *impl);
extern void tree_large_values(const ProllyImplInfo *impl);

/* engine_checks.c */
extern void engine_read_empty_tree(const ProllyImplInfo *impl);
extern void engine_read_own_writes(const ProllyImplInfo *impl);
extern void engine_seek_sees_later_writes(const ProllyImplInfo *impl);
extern void engine_subxact_abort_discards_writes(const ProllyImplInfo *impl);
extern void engine_create_and_truncate(const ProllyImplInfo *impl);
extern void engine_count_range_matches_scan(const ProllyImplInfo *impl);
extern void commit_publishes_in_enqueue_order(const ProllyImplInfo *impl);
extern void keying_round_trip(const ProllyImplInfo *impl);
extern void keying_keys_and_content(const ProllyImplInfo *impl);
extern void keying_description_reopens(const ProllyImplInfo *impl);
extern void keying_reads_dropped_key(const ProllyImplInfo *impl);
extern void keying_compare_after_type_dropped(const ProllyImplInfo *impl);
extern void keying_owns_its_memory(const ProllyImplInfo *impl);
extern void valenc_decodes_earlier_layouts(const ProllyImplInfo *impl);
extern void rowid_assign_then_resolve(const ProllyImplInfo *impl);
extern void rowid_rename_keeps_tid(const ProllyImplInfo *impl);
extern void rowid_find_sees_later_assign(const ProllyImplInfo *impl);
extern void rowid_never_reused(const ProllyImplInfo *impl);
extern void rowid_renew(const ProllyImplInfo *impl);

/* rootlayout_checks.c */
extern void rootlayout_empty(const ProllyImplInfo *impl);
extern void rootlayout_apply_matches_reference(const ProllyImplInfo *impl);
extern void rootlayout_history_independent(const ProllyImplInfo *impl);
extern void rootlayout_subroots_independent(const ProllyImplInfo *impl);
extern void rootlayout_diff_reports_changes(const ProllyImplInfo *impl);
extern void rootlayout_apply_rejects_invalid_refs(const ProllyImplInfo *impl);
extern void rootlayout_walk_reaches_trees(const ProllyImplInfo *impl);

#endif							/* TEST_PROLLY_H */
