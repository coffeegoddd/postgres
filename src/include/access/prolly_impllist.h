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

/* engines (specs/04-candidate-designs.md); all stubs so far */
PG_PROLLY_IMPL(ProllyEngineRoutine, prolly_engine_d1)
PG_PROLLY_IMPL(ProllyEngineRoutine, prolly_engine_d2)
PG_PROLLY_IMPL(ProllyEngineRoutine, prolly_engine_d3a)
PG_PROLLY_IMPL(ProllyEngineRoutine, prolly_engine_d3b)
PG_PROLLY_IMPL(ProllyEngineRoutine, prolly_engine_d4)

/* coordination modules; all stubs so far */
PG_PROLLY_IMPL(ProllyCoordRoutine, prolly_coord_k1)
PG_PROLLY_IMPL(ProllyCoordRoutine, prolly_coord_k2)
PG_PROLLY_IMPL(ProllyCoordRoutine, prolly_coord_k3)

/* foundation implementations (roadmap step 4a) */
PG_PROLLY_IMPL(ProllyHashRoutine, prolly_hash_sha256)
PG_PROLLY_IMPL(ProllyNodeFormatRoutine, prolly_nodeformat_basic)
PG_PROLLY_IMPL(ProllyChunkerRoutine, prolly_chunker_keyhash)
PG_PROLLY_IMPL(ProllyKeyEncoderRoutine, prolly_keyenc_ordered)
PG_PROLLY_IMPL(ProllyValueEncoderRoutine, prolly_valenc_heaptuple)

/* node storage (roadmap step 4b) */
PG_PROLLY_IMPL(ProllyNodeStoreRoutine, prolly_nodestore_global)
PG_PROLLY_IMPL(ProllyLogRoutine, prolly_log_chunks)

/* stubs of the foundation and version-control interfaces */
PG_PROLLY_IMPL(ProllyNodeFormatRoutine, prolly_nodeformat_stub)
PG_PROLLY_IMPL(ProllyChunkerRoutine, prolly_chunker_stub)
PG_PROLLY_IMPL(ProllyHashRoutine, prolly_hash_stub)
PG_PROLLY_IMPL(ProllyKeyEncoderRoutine, prolly_keyenc_stub)
PG_PROLLY_IMPL(ProllyValueEncoderRoutine, prolly_valenc_stub)
PG_PROLLY_IMPL(ProllyNodeStoreRoutine, prolly_nodestore_stub)
PG_PROLLY_IMPL(ProllyAllocatorRoutine, prolly_alloc_stub)
PG_PROLLY_IMPL(ProllyReclaimRoutine, prolly_reclaim_stub)
PG_PROLLY_IMPL(ProllyLogRoutine, prolly_log_stub)
PG_PROLLY_IMPL(ProllyCommitRoutine, prolly_commit_stub)
PG_PROLLY_IMPL(ProllyRowIdRoutine, prolly_rowid_stub)
PG_PROLLY_IMPL(ProllyRefsRoutine, prolly_refs_stub)
PG_PROLLY_IMPL(ProllyTransferRoutine, prolly_transfer_stub)
PG_PROLLY_IMPL(ProllyMergeRoutine, prolly_merge_stub)
