-- The chunk store conformance suite against "tier", the history store's
-- tier: its index on the server, its chunks' bytes in pack objects (here a
-- directory in the data directory), and its transactional part.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET chunk_free_in_background = off;
SET history_tier_location = 'pg_test_history_tier';
\set store tier
\getenv abs_srcdir PG_ABS_SRCDIR
\set suite :abs_srcdir '/sql/chunk_store_suite.inc'
\i :suite
\set suite :abs_srcdir '/sql/chunk_store_transactional.inc'
\i :suite

-- what the tier keeps: a pack a transaction that put chunks, written as it
-- commits; none of one that aborted
SELECT test_chunk_format_refs(false);
-- (the packs the suite left all dead removed first)
VACUUM pg_history.tier_index;
SELECT count(*) AS packs0 FROM test_os_list('directory', 'pg_test_history_tier', 'packs/') \gset
BEGIN;
SELECT (test_cs_put('tier', convert_to('tier-a' || g, 'UTF8'))).stored
FROM generate_series(1, 3) g;
COMMIT;
SELECT count(*) - :packs0 AS one_pack_more FROM test_os_list('directory', 'pg_test_history_tier', 'packs/');
BEGIN;
SELECT (test_cs_put('tier', convert_to('tier-gone', 'UTF8'))).stored;
ROLLBACK;
SELECT count(*) - :packs0 AS still_one_more FROM test_os_list('directory', 'pg_test_history_tier', 'packs/');
SELECT test_cs_has('tier', substr(sha256(convert_to('tier-gone', 'UTF8')), 1, 20)) AS gone;
-- read back from its pack
SELECT convert_from(test_cs_get('tier', substr(sha256(convert_to('tier-a2', 'UTF8')), 1, 20)), 'UTF8') AS read_back;
-- the pack's chunks freed: VACUUM takes their entries out, the pack all
-- dead, its object removed
SELECT test_cs_free('tier', ARRAY(SELECT substr(sha256(convert_to('tier-a' || g, 'UTF8')), 1, 20)
	FROM generate_series(1, 3) g)) AS freed;
VACUUM pg_history.tier_index;
SELECT count(*) - :packs0 AS pack_removed FROM test_os_list('directory', 'pg_test_history_tier', 'packs/');
SELECT ok FROM test_cs_check('tier');
