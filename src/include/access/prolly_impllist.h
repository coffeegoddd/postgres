/*---------------------------------------------------------------------------
 * prolly_impllist.h
 *
 * The built-in prolly implementations.  This is the one place outside an
 * implementation that names it (the composition root, like rmgrlist.h).
 *
 * PG_PROLLY_IMPL(RoutineType, routine_symbol)
 *
 * Each entry declares "extern const RoutineType routine_symbol;", defined
 * by the implementation's own source file.  Implementations are found by
 * the interface and name in their ProllyImplInfo, never by position here.
 *
 * src/include/access/prolly_impllist.h
 *---------------------------------------------------------------------------
 */

/* there is deliberately not an #ifndef PROLLY_IMPLLIST_H here */

/* foundation implementations */
PG_PROLLY_IMPL(ProllyHashRoutine, prolly_hash_sha256)
PG_PROLLY_IMPL(ProllyHashRoutine, prolly_hash_blake3)
PG_PROLLY_IMPL(ProllyChunkerRoutine, prolly_chunker_bytes)
PG_PROLLY_IMPL(ProllyChunkFormatRoutine, prolly_chunkformat_basic)
PG_PROLLY_IMPL(ProllyTableFileRoutine, prolly_tablefile_basic)
PG_PROLLY_IMPL(ProllyChangeFinderRoutine, prolly_changes_pages)
PG_PROLLY_IMPL(ProllyChangeFinderRoutine, prolly_changes_log)
PG_PROLLY_IMPL(ProllyChangeLogRoutine, prolly_changelog_relation)
PG_PROLLY_IMPL(ProllyMovePlannerRoutine, prolly_moveplan_costs)
PG_PROLLY_IMPL(ProllyKeyEncoderRoutine, prolly_keyenc_ordered)
PG_PROLLY_IMPL(ProllyValueEncoderRoutine, prolly_valenc_canonical)
PG_PROLLY_IMPL(ProllyKeyingRoutine, prolly_keying_pk_or_prefix)

/* history */
PG_PROLLY_IMPL(ProllyChunkStoreRoutine, prolly_chunkstore_journal)
PG_PROLLY_IMPL(ProllyTreeRoutine, prolly_tree_cow)
PG_PROLLY_IMPL(ProllyRefsRoutine, prolly_refs_copies)
PG_PROLLY_IMPL(ProllyMergeRoutine, prolly_merge_folded)
PG_PROLLY_IMPL(ProllyCanonRoutine, prolly_canon_folded)
PG_PROLLY_IMPL(ProllyTransferRoutine, prolly_transfer_chunks)
PG_PROLLY_IMPL(ProllyRemoteRoutine, prolly_remote_file)
PG_PROLLY_IMPL(ProllyRemoteRoutine, prolly_remote_postgresql)
PG_PROLLY_IMPL(ProllyRemoteRoutine, prolly_remote_postgres)
PG_PROLLY_IMPL(ProllyReclaimRoutine, prolly_reclaim_copying)
PG_PROLLY_IMPL(ProllyValueMemoRoutine, prolly_memo_shared)
PG_PROLLY_IMPL(ProllyWritersRoutine, prolly_writers_modes)
PG_PROLLY_IMPL(ProllyMatCacheRoutine, prolly_matcache_branches)
PG_PROLLY_IMPL(ProllyJsonTreeRoutine, prolly_jsontree_paths)

/* row appliers; the stub is the interface's */
PG_PROLLY_IMPL(ProllyApplierRoutine, prolly_applier_replication)
PG_PROLLY_IMPL(ProllyApplierRoutine, prolly_applier_stub)

/* stubs */
PG_PROLLY_IMPL(ProllyChunkerRoutine, prolly_chunker_stub)
PG_PROLLY_IMPL(ProllyHashRoutine, prolly_hash_stub)
PG_PROLLY_IMPL(ProllyKeyEncoderRoutine, prolly_keyenc_stub)
PG_PROLLY_IMPL(ProllyValueEncoderRoutine, prolly_valenc_stub)
PG_PROLLY_IMPL(ProllyReclaimRoutine, prolly_reclaim_stub)
PG_PROLLY_IMPL(ProllyRefsRoutine, prolly_refs_stub)
PG_PROLLY_IMPL(ProllyTransferRoutine, prolly_transfer_stub)
PG_PROLLY_IMPL(ProllyMergeRoutine, prolly_merge_stub)
PG_PROLLY_IMPL(ProllyTableFileRoutine, prolly_tablefile_stub)
PG_PROLLY_IMPL(ProllyChangeFinderRoutine, prolly_changes_stub)
PG_PROLLY_IMPL(ProllyCanonRoutine, prolly_canon_stub)
PG_PROLLY_IMPL(ProllyTreeRoutine, prolly_tree_stub)
PG_PROLLY_IMPL(ProllyKeyingRoutine, prolly_keying_stub)
PG_PROLLY_IMPL(ProllyValueMemoRoutine, prolly_memo_stub)
PG_PROLLY_IMPL(ProllyWritersRoutine, prolly_writers_stub)
PG_PROLLY_IMPL(ProllyChangeLogRoutine, prolly_changelog_stub)
PG_PROLLY_IMPL(ProllyMovePlannerRoutine, prolly_moveplan_stub)
PG_PROLLY_IMPL(ProllyMatCacheRoutine, prolly_matcache_stub)
PG_PROLLY_IMPL(ProllyJsonTreeRoutine, prolly_jsontree_stub)
