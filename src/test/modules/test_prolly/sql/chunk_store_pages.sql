-- The chunk store conformance suite against "pages", the history store of
-- this database, and its transactional part.
CREATE EXTENSION IF NOT EXISTS test_prolly;
-- (chunks nothing holds kept: no freer in the background)
SET chunk_free_in_background = off;
-- every store registered runs the suite, in a file chunk_store_<name>.sql
SELECT * FROM test_chunk_stores() ORDER BY name;
\set store pages
\getenv abs_srcdir PG_ABS_SRCDIR
\set suite :abs_srcdir '/sql/chunk_store_suite.inc'
\i :suite
\set suite :abs_srcdir '/sql/chunk_store_transactional.inc'
\i :suite
