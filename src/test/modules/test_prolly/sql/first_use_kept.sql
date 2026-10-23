-- A branch's first use makes its tables of the pages the branch it was
-- made from keeps as of its head (#489): each shares a snapshot's pages,
-- O(1), with no row written back and no lock on the source's writers.
-- What it holds is the head's rows, whatever the source's working set has
-- since; a table whose pages aren't kept (made since the commit, its
-- storage replaced since) is copied as before and moved to the head.

SELECT current_database() AS home \gset
CREATE DATABASE first_use_kept;
\c first_use_kept
SET client_min_messages = warning;
CREATE EXTENSION amcheck;
RESET client_min_messages;

CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM pg_stat_prolly
	WHERE name IN ('apply_rows', 'first_use_tables_kept', 'bmap_map_pages_copied')
$$;
CREATE FUNCTION since(before jsonb) RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT key, value::bigint - (before ->> key)::bigint
	FROM jsonb_each_text(counted()) ORDER BY 1
$$;

CREATE SCHEMA fk VERSIONED;
CREATE TABLE fk.t (id bigint PRIMARY KEY, v int NOT NULL, w text NOT NULL);
CREATE INDEX t_v ON fk.t (v);
CREATE TABLE fk.k (k int, note text);
CREATE TABLE fk.s (id serial PRIMARY KEY, x text);
INSERT INTO fk.t SELECT g, g % 7, repeat(md5(g::text), 1 + g % 50)
FROM generate_series(1, 5000) g;
INSERT INTO fk.k SELECT g % 50, 'n' FROM generate_series(1, 1000) g;
INSERT INTO fk.s (x) SELECT 'a' FROM generate_series(1, 10);
SELECT dolt_commit('-Am', 'load') IS NOT NULL AS committed;
CREATE FUNCTION rows_digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5((SELECT string_agg(t::text, ',' ORDER BY id) FROM fk.t) || '|' ||
			   (SELECT string_agg(k::text || ':' || n, ',' ORDER BY k)
				FROM (SELECT k, count(*) n FROM fk.k GROUP BY k) g) || '|' ||
			   (SELECT string_agg(s::text, ',' ORDER BY id) FROM fk.s))
$$;
SELECT rows_digest() AS at_load \gset

-- main's working set changes after its commit; a branch made at the head
UPDATE fk.t SET v = -1 WHERE id <= 300;
DELETE FROM fk.k WHERE k = 1;
INSERT INTO fk.s (x) VALUES ('main');
SELECT rows_digest() AS main_dirty \gset
SELECT dolt_branch('b1');
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_checkout('b1');
SELECT pg_stat_force_next_flush();
-- (each table made of kept pages, no row written, no map page copied
-- but the copies' own)
SELECT * FROM since(:'before');
SELECT rows_digest() = :'at_load' AS at_head, NOT EXISTS (SELECT FROM dolt_status) AS clean;
SET enable_seqscan = off;
SELECT count(*) FROM fk.t WHERE v = -1;
SELECT count(*) FROM fk.t WHERE id = 7;
RESET enable_seqscan;
SELECT nextval('fk.s_id_seq') AS next_on_branch;
SELECT dolt_checkout('main');
SELECT rows_digest() = :'main_dirty' AS main_as_it_was;
SELECT bool_and(bt_index_check(indexrelid, true) IS NOT NULL) AS indexes_valid
FROM pg_index WHERE indrelid IN ('fk.t'::regclass, 'fk.s'::regclass);

-- a table made since the commit, and one whose storage was replaced
-- since: copied as before, then moved to the head
CREATE TABLE fk.n (id int PRIMARY KEY);
TRUNCATE fk.k;
SELECT dolt_branch('b2');
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_checkout('b2');
SELECT pg_stat_force_next_flush();
SELECT name, value > 0 AS some FROM since(:'before');
SELECT rows_digest() = :'at_load' AS at_head, NOT EXISTS (SELECT FROM dolt_status) AS clean,
	to_regclass('fk.n') IS NULL AS no_new_table;
SELECT dolt_checkout('main');

-- the branch keeps its own pages as of its head: a reset there gives
-- them back
SELECT dolt_checkout('b1');
UPDATE fk.t SET v = 5 WHERE id <= 10;
SELECT pg_stat_force_next_flush();
SELECT (counted() ->> 'apply_rows')::bigint AS applied \gset
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT (counted() ->> 'apply_rows')::bigint - :applied AS reset_applied;
SELECT rows_digest() = :'at_load' AS at_head;
SELECT dolt_checkout('main');

-- main's later commits retire and let go of the pages it kept as of the
-- first; a branch made at that commit is made of another's kept as of it (b1's)
SELECT dolt_reset('--hard');
DO $$
BEGIN
	FOR i IN 1..4 LOOP
		UPDATE fk.t SET v = 100 + i WHERE id = 2;
		PERFORM dolt_commit('-am', 'step ' || i);
	END LOOP;
END $$;
SELECT dolt_branch('b3', (SELECT commit_hash FROM dolt_log WHERE message = 'load'));
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_checkout('b3');
SELECT pg_stat_force_next_flush();
SELECT name, value FROM since(:'before') WHERE name = 'first_use_tables_kept';
SELECT rows_digest() = :'at_load' AS at_load;
SELECT dolt_checkout('main');

\c :home
DROP DATABASE first_use_kept;
