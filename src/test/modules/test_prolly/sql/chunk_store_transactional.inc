-- The transactional stores' part of the suite: a put, a root's change and
-- a free are their transaction's, gone if it aborts.
SELECT test_chunk_format_refs(true);
BEGIN;
SELECT pg_temp.put('cs-tx-f') AS f;
ROLLBACK;
SELECT test_cs_has(:'store', pg_temp.h('cs-tx-f')) AS f_after_abort;
SELECT pg_temp.put('cs-tx-g') AS g;
BEGIN;
SELECT test_cs_ref_root(:'store', pg_temp.h('cs-tx-g'));
SELECT roots FROM test_cs_counts(:'store', pg_temp.h('cs-tx-g'));
ROLLBACK;
SELECT roots FROM test_cs_counts(:'store', pg_temp.h('cs-tx-g'));
BEGIN;
SELECT pg_temp.free('cs-tx-g') AS g_freed;
SELECT test_cs_has(:'store', pg_temp.h('cs-tx-g')) AS g_in_its_transaction;
ROLLBACK;
SELECT test_cs_has(:'store', pg_temp.h('cs-tx-g')) AS g_after_abort;
SELECT pg_temp.free('cs-tx-g') AS g_freed;
SELECT test_cs_check(:'store') AS audit;
SELECT test_chunk_format_refs(false);
