-- A clone copies the remote's files whole, each chunk re-hashed as it
-- comes, out of what the store reads until every reference in them is
-- resolved and every commit checked; then the store takes them all at once.
-- It brings the remote's garbage too.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT value AS fmt FROM test_fold_items() WHERE key = '!:format' \gset
SELECT current_database() AS db \gset
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/whole-' || txid_current() AS run \gset
\set url :run '/a'
\set url_broken :run '/broken'

\c postgres
CREATE DATABASE whole_from;
\c whole_from
CREATE EXTENSION test_prolly;
CREATE SCHEMA wf VERSIONED;
CREATE TABLE wf.t (id int PRIMARY KEY, v text);
INSERT INTO wf.t SELECT g, 'v' || g FROM generate_series(1, 300) g;
SELECT length(dolt_commit('-Am', 'wf 1')::text) AS committed;
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main') AS pushed;
UPDATE wf.t SET v = 'changed' WHERE id <= 10;
SELECT length(dolt_commit('-am', 'wf 2')::text) AS committed;
SELECT dolt_push('origin', 'main') AS pushed;
-- (a chunk nothing names, in a file of its own)
SELECT test_remote_put(:'url', test_content_hash('garbage on the remote'),
	'garbage on the remote');

\c postgres
CREATE DATABASE whole_into;
\c whole_into
CREATE EXTENSION test_prolly;
SELECT dolt_clone(:'url');
SELECT count(*), count(*) FILTER (WHERE v = 'changed') AS changed FROM wf.t;
SELECT count(*) AS commits FROM dolt_log('main');
-- the remote's three files, taken whole: the garbage with them
SELECT count(*) FILTER (WHERE name LIKE 'prolly\_received\_%') AS received_files
FROM test_chunk_table_files();
SELECT test_chunk_has(test_content_hash('garbage on the remote')) AS garbage_came;

-- A remote whose chunk names one it lacks: refused, nothing named (the
-- file copied stays, out of what the store reads, for a clone tried again).
-- (the node made where what it names is, and sent without it)
\c whole_from
SELECT test_chunk_put_node(1, ARRAY[test_chunk_put('nowhere')]) AS bad \gset
SELECT test_remote_put(:'url_broken', :'bad', test_chunk_get(:'bad'));
SELECT test_remote_set_head(:'url_broken', 'main', :'bad', :'fmt');
\c postgres
CREATE DATABASE whole_broken;
\c whole_broken
CREATE EXTENSION test_prolly;
SELECT set_config('test.url', :'url_broken', false) IS NOT NULL AS set;
DO $$
BEGIN
	PERFORM dolt_clone(current_setting('test.url'));
EXCEPTION WHEN OTHERS THEN
	RAISE NOTICE 'refused: %', regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g');
END $$;
SELECT count(*) AS named FROM test_chunk_table_files();
SELECT count(*) AS kept_for_a_retry
FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'whole_broken') db,
	pg_ls_dir(d) f
WHERE f LIKE 'prolly\_received\_%';

\c postgres
DROP DATABASE whole_from;
DROP DATABASE whole_into;
DROP DATABASE whole_broken;
\c :db
