-- The chunk store conformance suite against "memory", a backend's store in
-- memory (not transactional).
CREATE EXTENSION IF NOT EXISTS test_prolly;
SELECT test_cs_reset_memory();
\set store memory
\getenv abs_srcdir PG_ABS_SRCDIR
\set suite :abs_srcdir '/sql/chunk_store_suite.inc'
\i :suite
