-- The commits a branch keeps its pages as of are kept states of the
-- materialization cache, as kept checkouts are: a move of another branch
-- (a reset, a checkout's move, a first use's) gives a table whose rows
-- differ from its target's storage made of a kept commit's snapshots
-- where that commit holds the target's table, O(1), rather than writing
-- the rows; and a first use at a kept commit is made of its pages.

SELECT current_database() AS home \gset
CREATE DATABASE kept_commit_shares;
\c kept_commit_shares
SET client_min_messages = warning;
CREATE EXTENSION amcheck;
RESET client_min_messages;

CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM pg_stat_prolly
	WHERE name IN ('apply_rows', 'move_tables_shared', 'relations_restored',
				   'first_use_tables_kept')
$$;
CREATE FUNCTION since(before jsonb) RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT key, value::bigint - (before ->> key)::bigint
	FROM jsonb_each_text(counted()) ORDER BY 1
$$;

CREATE SCHEMA ks VERSIONED;
CREATE TABLE ks.t (id bigint PRIMARY KEY, v int NOT NULL, w text NOT NULL);
CREATE INDEX t_v ON ks.t (v);
INSERT INTO ks.t SELECT g, g % 10, md5(g::text) FROM generate_series(1, 20000) g;
CREATE FUNCTION digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM ks.t t
$$;
SELECT dolt_commit('-Am', 'c0') IS NOT NULL AS committed;
SELECT dolt_branch('b');

-- main moves on: a commit (its pages kept as of it), then changes after it,
-- so that main's working set is no longer that commit's
UPDATE ks.t SET v = v + 1 WHERE id <= 5000;
SELECT dolt_commit('-am', 'c1') IS NOT NULL AS committed;
SELECT commit_hash AS c1 FROM dolt_log WHERE message = 'c1' \gset
SELECT digest() AS at_c1 \gset
UPDATE ks.t SET v = v + 2 WHERE id <= 5000;
SELECT digest() AS main_dirty \gset

-- b, used at c0 and changed, is reset to c1: the table's storage is made
-- of main's kept pages as of c1, no row written
SELECT dolt_checkout('b');
UPDATE ks.t SET v = 0 WHERE id <= 6000;
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'c1');
SELECT pg_stat_force_next_flush();
SELECT name, value > 0 AS some FROM since(:'before');
SELECT digest() = :'at_c1' AS at_c1, NOT EXISTS (SELECT FROM dolt_status) AS clean;
SET enable_seqscan = off;
SELECT count(*) AS by_index FROM ks.t WHERE v = 0;
RESET enable_seqscan;
SELECT bool_and(bt_index_check(indexrelid, true) IS NOT NULL) AS indexes_valid
FROM pg_index WHERE indrelid = 'ks.t'::regclass;

-- b's own writes go on from there; main is as it was
UPDATE ks.t SET v = 9 WHERE id = 1;
SELECT dolt_commit('-am', 'on b') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT digest() = :'main_dirty' AS main_as_it_was;

-- a branch made at c1 is made of main's pages kept as of it (a kept
-- commit, found through the materialization cache)
SELECT dolt_branch('c', :'c1');
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_checkout('c');
SELECT pg_stat_force_next_flush();
SELECT name, value FROM since(:'before') WHERE name IN ('apply_rows', 'first_use_tables_kept');
SELECT digest() = :'at_c1' AS at_c1;
SELECT dolt_checkout('main');

-- a commit main keeps no pages of (prolly.kept_commits = 0 as it commits):
-- the reset to it, with the setting back, writes the rows; main's kept c1
-- is visited, but doesn't hold the commit's table
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'pages_kept') AS kept_before \gset
SET prolly.kept_commits = 0;
UPDATE ks.t SET v = v + 1 WHERE id <= 5000;
SELECT dolt_commit('-am', 'c2') IS NOT NULL AS committed;
RESET prolly.kept_commits;
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'pages_kept') - :kept_before AS c2_pages_kept;
SELECT commit_hash AS c2 FROM dolt_log WHERE message = 'c2' \gset
UPDATE ks.t SET v = 7 WHERE id <= 5000;
SELECT dolt_checkout('b');
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'c2');
SELECT pg_stat_force_next_flush();
SELECT name, value > 0 AS some FROM since(:'before');
SELECT dolt_checkout('main');

-- the same with the commit kept: shared
SELECT dolt_reset('--hard');
UPDATE ks.t SET v = v + 1 WHERE id <= 5000;
SELECT dolt_commit('-am', 'c3') IS NOT NULL AS committed;
SELECT commit_hash AS c3 FROM dolt_log WHERE message = 'c3' \gset
UPDATE ks.t SET v = 7 WHERE id <= 5000;
SELECT dolt_checkout('b');
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'c3');
SELECT pg_stat_force_next_flush();
SELECT name, value > 0 AS some FROM since(:'before');
SELECT dolt_checkout('main');

\c :home
DROP DATABASE kept_commit_shares;
