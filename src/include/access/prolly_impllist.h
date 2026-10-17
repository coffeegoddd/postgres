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

/* engines; d1 is built, the others are stubs */
PG_PROLLY_IMPL(ProllyEngineRoutine, prolly_engine_d1)
PG_PROLLY_IMPL(ProllyEngineRoutine, prolly_engine_d2)
PG_PROLLY_IMPL(ProllyEngineRoutine, prolly_engine_d3a)
PG_PROLLY_IMPL(ProllyEngineRoutine, prolly_engine_d3b)
PG_PROLLY_IMPL(ProllyEngineRoutine, prolly_engine_d4)

/* coordination modules; K1-K3 are stubs so far */
PG_PROLLY_IMPL(ProllyCoordRoutine, prolly_coord_k1)
PG_PROLLY_IMPL(ProllyCoordRoutine, prolly_coord_k2)
PG_PROLLY_IMPL(ProllyCoordRoutine, prolly_coord_k3)
PG_PROLLY_IMPL(ProllyCoordRoutine, prolly_coord_single_writer)

/* foundation implementations */
PG_PROLLY_IMPL(ProllyHashRoutine, prolly_hash_sha256)
PG_PROLLY_IMPL(ProllyNodeFormatRoutine, prolly_nodeformat_basic)
PG_PROLLY_IMPL(ProllyChunkerRoutine, prolly_chunker_keyhash)
PG_PROLLY_IMPL(ProllyChunkerRoutine, prolly_chunker_bytes)
PG_PROLLY_IMPL(ProllyChunkFormatRoutine, prolly_chunkformat_basic)
PG_PROLLY_IMPL(ProllyTableFileRoutine, prolly_tablefile_basic)
PG_PROLLY_IMPL(ProllyChangeFinderRoutine, prolly_changes_pages)
PG_PROLLY_IMPL(ProllyKeyEncoderRoutine, prolly_keyenc_ordered)
PG_PROLLY_IMPL(ProllyValueEncoderRoutine, prolly_valenc_heaptuple)
PG_PROLLY_IMPL(ProllyValueEncoderRoutine, prolly_valenc_canonical)
PG_PROLLY_IMPL(ProllyKeyingRoutine, prolly_keying_pk_or_content)
PG_PROLLY_IMPL(ProllyKeyingRoutine, prolly_keying_pk_or_prefix)
PG_PROLLY_IMPL(ProllyIndexKeyingRoutine, prolly_ixkeying_columns_then_row)

/* node storage */
PG_PROLLY_IMPL(ProllyNodeStoreRoutine, prolly_nodestore_global)
PG_PROLLY_IMPL(ProllyChunkStoreRoutine, prolly_chunkstore_journal)
PG_PROLLY_IMPL(ProllyLogRoutine, prolly_log_chunks)

/* trees */
PG_PROLLY_IMPL(ProllyTreeRoutine, prolly_tree_cow)

/* commit pipelines and row identity */
PG_PROLLY_IMPL(ProllyCommitRoutine, prolly_commit_serial)
PG_PROLLY_IMPL(ProllyRowIdRoutine, prolly_rowid_counter)

/* root layouts */
PG_PROLLY_IMPL(ProllyRootLayoutRoutine, prolly_rootlayout_subroots)

/* row appliers; the stub is the interface's */
PG_PROLLY_IMPL(ProllyApplierRoutine, prolly_applier_replication)
PG_PROLLY_IMPL(ProllyApplierRoutine, prolly_applier_stub)

/* stubs of the foundation and version-control interfaces */
PG_PROLLY_IMPL(ProllyNodeFormatRoutine, prolly_nodeformat_stub)
PG_PROLLY_IMPL(ProllyChunkerRoutine, prolly_chunker_stub)
PG_PROLLY_IMPL(ProllyHashRoutine, prolly_hash_stub)
PG_PROLLY_IMPL(ProllyKeyEncoderRoutine, prolly_keyenc_stub)
PG_PROLLY_IMPL(ProllyValueEncoderRoutine, prolly_valenc_stub)
PG_PROLLY_IMPL(ProllyNodeStoreRoutine, prolly_nodestore_stub)
PG_PROLLY_IMPL(ProllyAllocatorRoutine, prolly_alloc_stub)
PG_PROLLY_IMPL(ProllyReclaimRoutine, prolly_reclaim_stub)
PG_PROLLY_IMPL(ProllyReclaimRoutine, prolly_reclaim_mark_sweep)
PG_PROLLY_IMPL(ProllyLogRoutine, prolly_log_stub)
PG_PROLLY_IMPL(ProllyCommitRoutine, prolly_commit_stub)
PG_PROLLY_IMPL(ProllyRowIdRoutine, prolly_rowid_stub)
PG_PROLLY_IMPL(ProllyRefsRoutine, prolly_refs_stub)
PG_PROLLY_IMPL(ProllyRefsRoutine, prolly_refs_tree)
PG_PROLLY_IMPL(ProllyRefsRoutine, prolly_refs_copies)
PG_PROLLY_IMPL(ProllyTransferRoutine, prolly_transfer_stub)
PG_PROLLY_IMPL(ProllyTransferRoutine, prolly_transfer_chunks)
PG_PROLLY_IMPL(ProllyRemoteRoutine, prolly_remote_file)
PG_PROLLY_IMPL(ProllyMergeRoutine, prolly_merge_stub)
PG_PROLLY_IMPL(ProllyTableFileRoutine, prolly_tablefile_stub)
PG_PROLLY_IMPL(ProllyChangeFinderRoutine, prolly_changes_stub)
PG_PROLLY_IMPL(ProllyMergeRoutine, prolly_merge_rows)
PG_PROLLY_IMPL(ProllyMergeRoutine, prolly_merge_folded)
PG_PROLLY_IMPL(ProllyCanonRoutine, prolly_canon_stub)
PG_PROLLY_IMPL(ProllyCanonRoutine, prolly_canon_catalogs)
PG_PROLLY_IMPL(ProllyCanonRoutine, prolly_canon_folded)
PG_PROLLY_IMPL(ProllyTreeRoutine, prolly_tree_stub)
PG_PROLLY_IMPL(ProllyRootLayoutRoutine, prolly_rootlayout_stub)
PG_PROLLY_IMPL(ProllyKeyingRoutine, prolly_keying_stub)
PG_PROLLY_IMPL(ProllyIndexKeyingRoutine, prolly_ixkeying_stub)
