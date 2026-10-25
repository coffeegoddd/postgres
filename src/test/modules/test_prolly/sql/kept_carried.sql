-- A restore gives each table it restores new storage, made of the kept
-- snapshot, and the storage it replaces goes as the move commits.  The
-- other commits' snapshots of the table were in that storage: they are
-- carried into the new storage (O(1) each), so those commits stay kept,
-- and resets back and forth between two commits restore every time,
-- writing no row.

SELECT current_database() AS home \gset
CREATE DATABASE kept_carried;
ALTER DATABASE kept_carried SET prolly.restore_min_changes = 0;
\c kept_carried
SET client_min_messages = warning;
CREATE EXTENSION amcheck;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

CREATE FUNCTION counted(n text) RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce((SELECT value FROM pg_stat_prolly p WHERE p.name = n), 0)
$$;
CREATE SCHEMA kc VERSIONED;
CREATE TABLE kc.t (id int PRIMARY KEY, v int NOT NULL, big text);
ALTER TABLE kc.t ALTER COLUMN big SET STORAGE EXTERNAL;
CREATE INDEX t_v ON kc.t (v);
INSERT INTO kc.t SELECT g, 0, CASE WHEN g <= 20 THEN repeat(md5(g::text), 200) END
FROM generate_series(1, 3000) g;
CREATE FUNCTION rows_digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(id || ':' || v || ':' || coalesce(md5(big), ''), ','
						  ORDER BY id))
	FROM kc.t
$$;
CREATE FUNCTION indexes_valid() RETURNS bool LANGUAGE sql AS $$
	SELECT bool_and(bt_index_check(i.indexrelid, true) IS NOT NULL)
	FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'btree'
	WHERE i.indrelid IN ('kc.t'::regclass,
						 (SELECT reltoastrelid FROM pg_class WHERE oid = 'kc.t'::regclass))
$$;
SELECT (dolt_commit('-Am', 'a'))[1] AS a \gset
SELECT rows_digest() AS at_a \gset
UPDATE kc.t SET v = 1 WHERE id <= 300;
UPDATE kc.t SET big = repeat('b', 5000) WHERE id <= 5;
SELECT (dolt_commit('-am', 'b'))[1] AS b \gset
SELECT rows_digest() AS at_b \gset

-- back and forth, with rows changed each time: every reset gives the
-- pages back, none written
CREATE TABLE trips (n int, restored bool, applied bigint, as_target bool,
					valid bool);
UPDATE kc.t SET v = v + 10 WHERE id % 7 = 1;
SELECT pg_stat_force_next_flush();
SELECT counted('relations_restored') AS r, counted('apply_rows') AS ap \gset
SELECT dolt_reset('--hard', :'a');
SELECT pg_stat_force_next_flush();
INSERT INTO trips VALUES (1, counted('relations_restored') > :r,
						  counted('apply_rows') - :ap,
						  rows_digest() = :'at_a', indexes_valid());
UPDATE kc.t SET v = v + 10 WHERE id % 7 = 2;
SELECT pg_stat_force_next_flush();
SELECT counted('relations_restored') AS r, counted('apply_rows') AS ap \gset
SELECT dolt_reset('--hard', :'b');
SELECT pg_stat_force_next_flush();
INSERT INTO trips VALUES (2, counted('relations_restored') > :r,
						  counted('apply_rows') - :ap,
						  rows_digest() = :'at_b', indexes_valid());
UPDATE kc.t SET v = v + 10 WHERE id % 7 = 3;
SELECT pg_stat_force_next_flush();
SELECT counted('relations_restored') AS r, counted('apply_rows') AS ap \gset
SELECT dolt_reset('--hard', :'a');
SELECT pg_stat_force_next_flush();
INSERT INTO trips VALUES (3, counted('relations_restored') > :r,
						  counted('apply_rows') - :ap,
						  rows_digest() = :'at_a', indexes_valid());
UPDATE kc.t SET v = v + 10 WHERE id % 7 = 4;
SELECT pg_stat_force_next_flush();
SELECT counted('relations_restored') AS r, counted('apply_rows') AS ap \gset
SELECT dolt_reset('--hard', :'b');
SELECT pg_stat_force_next_flush();
INSERT INTO trips VALUES (4, counted('relations_restored') > :r,
						  counted('apply_rows') - :ap,
						  rows_digest() = :'at_b', indexes_valid());
UPDATE kc.t SET v = v + 10 WHERE id % 7 = 5;
SELECT pg_stat_force_next_flush();
SELECT counted('relations_restored') AS r, counted('apply_rows') AS ap \gset
SELECT dolt_reset('--hard', :'a');
SELECT pg_stat_force_next_flush();
INSERT INTO trips VALUES (5, counted('relations_restored') > :r,
						  counted('apply_rows') - :ap,
						  rows_digest() = :'at_a', indexes_valid());
UPDATE kc.t SET v = v + 10 WHERE id % 7 = 6;
SELECT pg_stat_force_next_flush();
SELECT counted('relations_restored') AS r, counted('apply_rows') AS ap \gset
SELECT dolt_reset('--hard', :'b');
SELECT pg_stat_force_next_flush();
INSERT INTO trips VALUES (6, counted('relations_restored') > :r,
						  counted('apply_rows') - :ap,
						  rows_digest() = :'at_b', indexes_valid());
SELECT * FROM trips ORDER BY n;

-- each relation keeps a snapshot for each commit kept, no more
SELECT count(*) AS t_snapshots
FROM test_bmap_snapshots(pg_relation_filenode('kc.t'));
SELECT count(*) AS t_v_snapshots
FROM test_bmap_snapshots(pg_relation_filenode('kc.t_v'));
SELECT count(*) AS kept FROM test_kept_commits('main');

-- found by the indexes as the rows are
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM kc.t WHERE v = 1;
SELECT count(*) FROM kc.t WHERE id <= 5 AND big = repeat('b', 5000);
RESET enable_seqscan;
RESET enable_bitmapscan;

-- every block the stores hold is held by a map or a snapshot kept
SELECT dolt_commit('--allow-empty', '-m', 'settled') IS NOT NULL AS committed;
SELECT freed FROM prolly_page_sweep();

\c :home
DROP DATABASE kept_carried;
