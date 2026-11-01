-- The transactional stores' part of the suite: a put, a root's change and
-- a free are their transaction's, gone, with what they counted, if it
-- aborts.
SELECT test_chunk_format_refs(true);
BEGIN;
SELECT pg_temp.put('cs-tx-f') AS f;
ROLLBACK;
SELECT pg_temp.has('cs-tx-f') AS f_after_abort;
-- a parent's put rolled back: its leaf's reference with it
SELECT pg_temp.put('cs-tx-g') AS g;
BEGIN;
SELECT pg_temp.put(pg_temp.parent('cs-tx-g')) AS parent;
SELECT pg_temp.refs('cs-tx-g') AS g;
ROLLBACK;
SELECT pg_temp.refs('cs-tx-g') AS g, pg_temp.has(pg_temp.parent('cs-tx-g')) AS parent;
-- roots taken, and let go of, rolled back
BEGIN;
SELECT pg_temp.root('cs-tx-g', true);
SELECT pg_temp.refs('cs-tx-g') AS g;
ROLLBACK;
SELECT pg_temp.refs('cs-tx-g') AS g;
SELECT pg_temp.root('cs-tx-g', true);
BEGIN;
SELECT pg_temp.root('cs-tx-g', false);
ROLLBACK;
SELECT pg_temp.refs('cs-tx-g') AS g;
SELECT pg_temp.root('cs-tx-g', false);
-- a free, and a cascade, rolled back
BEGIN;
SELECT pg_temp.free('cs-tx-g') AS g_freed;
SELECT pg_temp.has('cs-tx-g') AS g_in_its_transaction;
ROLLBACK;
SELECT pg_temp.has('cs-tx-g') AS g_after_abort;
SELECT pg_temp.put(pg_temp.parent('cs-tx-g')) AS parent;
BEGIN;
SELECT pg_temp.free(pg_temp.parent('cs-tx-g')) AS parent_and_g;
ROLLBACK;
SELECT pg_temp.has(pg_temp.parent('cs-tx-g')) AS parent, pg_temp.refs('cs-tx-g') AS g;
SELECT pg_temp.free(pg_temp.parent('cs-tx-g')) AS parent_and_g;
SELECT ok, why FROM test_cs_check(:'store');
SELECT test_chunk_format_refs(false);
