-- The chunk store conformance suite against "memory", a backend's store in
-- memory (not transactional).
CREATE EXTENSION IF NOT EXISTS test_prolly;
SELECT test_cs_reset_memory();
\set store memory
\getenv abs_srcdir PG_ABS_SRCDIR
\set suite :abs_srcdir '/sql/chunk_store_suite.inc'
\i :suite

-- not transactional: an abort leaves a put
BEGIN;
SELECT pg_temp.put('cs-mem-kept') AS kept;
ROLLBACK;
SELECT pg_temp.has('cs-mem-kept') AS kept_after_abort;
-- the audit finds a count wrong
SELECT pg_temp.put('cs-mem-miscounted') AS miscounted;
SELECT test_cs_memory_miscount(pg_temp.h('cs-mem-miscounted'));
SELECT ok, why FROM test_cs_check('memory');
SELECT test_cs_reset_memory();
