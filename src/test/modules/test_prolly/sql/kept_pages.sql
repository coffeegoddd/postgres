-- A branch's pages are kept as of the commits it was last at (#767): a hard
-- reset to one gives each table that changed since the pages kept (new
-- storage made of a snapshot of each relation stored with it), writing no
-- row and making no index entry, rather than writing the rows that differ
-- back.  What it leaves is what writing the rows left: the commit's rows,
-- every index valid, the change log folding from where it was.  A table
-- that can't be restored (its storage replaced since, an index made since,
-- pages not kept) has its rows written as before.

SELECT current_database() AS home \gset
CREATE DATABASE kept_pages;
-- (history older than the window keeps no roots: a window long enough
-- for all of this one's)
ALTER DATABASE kept_pages SET prolly.history_window = '1000 years';
-- (every changed table restored, however few its changes: the threshold
-- is tested at the end)
ALTER DATABASE kept_pages SET prolly.restore_min_changes = 0;
\c kept_pages
SET client_min_messages = warning;
CREATE EXTENSION amcheck;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

-- (the counters a move counts, read once the session's are flushed)
CREATE FUNCTION counted() RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT n, coalesce((SELECT p.value FROM pg_stat_prolly p WHERE p.name = n), -1)
	FROM unnest('{apply_rows,apply_index_tuples,relations_restored,pages_kept}'::text[]) n
$$;
CREATE FUNCTION since(before jsonb) RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(c.name, c.value - (before ->> c.name)::bigint)
	FROM counted() c
$$;
CREATE FUNCTION now_counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM counted()
$$;
CREATE SCHEMA kp VERSIONED;
CREATE TABLE kp.t (id bigint PRIMARY KEY, v int NOT NULL, w text NOT NULL)
	WITH (fillfactor = 50);
CREATE INDEX t_v ON kp.t (v);
CREATE UNIQUE INDEX t_w ON kp.t (w);
CREATE TABLE kp.k (k int, note text);		-- keyless: copies count
CREATE TABLE kp.big (id int PRIMARY KEY, big text);	-- TOAST
ALTER TABLE kp.big ALTER COLUMN big SET STORAGE EXTERNAL;
CREATE TABLE kp.other (id int PRIMARY KEY);	-- never changed
INSERT INTO kp.t SELECT g, g % 7, 'w' || g FROM generate_series(1, 5000) g;
INSERT INTO kp.k SELECT g % 50, 'n' FROM generate_series(1, 1000) g;
INSERT INTO kp.big SELECT g, repeat(md5(g::text), 200) FROM generate_series(1, 40) g;
INSERT INTO kp.other SELECT generate_series(1, 10);
-- every btree index of the versioned schema's tables checked, with every
-- row of its table found in it
CREATE FUNCTION indexes_valid() RETURNS bool LANGUAGE sql AS $$
	SELECT bool_and(bt_index_check(i.indexrelid, true) IS NOT NULL)
	FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'btree'
	WHERE i.indrelid IN (SELECT oid FROM pg_class
						 WHERE relnamespace = 'kp'::regnamespace)
$$;
-- the rows of the tables, digested (a keyless table's copies counted)
CREATE FUNCTION rows_digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5((SELECT string_agg(t::text, ',' ORDER BY id) FROM kp.t) || '|' ||
			   (SELECT string_agg(k::text || ':' || n, ',' ORDER BY k)
				FROM (SELECT k, count(*) n FROM kp.k GROUP BY k) g) || '|' ||
			   (SELECT string_agg(id || ':' || md5(big), ',' ORDER BY id) FROM kp.big))
$$;
-- the working set is the head's
CREATE FUNCTION clean() RETURNS bool LANGUAGE sql AS
$$SELECT NOT EXISTS (SELECT FROM dolt_status)$$;
-- the roots captured of a table's storage, held
CREATE FUNCTION kept(rel regclass) RETURNS bigint LANGUAGE sql AS
$$SELECT test_bmap_roots_of(pg_relation_filenode(rel))$$;

SELECT dolt_commit('-Am', 'load') IS NOT NULL AS committed;
SELECT rows_digest() AS at_load \gset
-- (each relation of each table kept, by one snapshot)
SELECT kept('kp.t') AS t, kept('kp.t_v') AS t_v, kept('kp.big') AS big;

-- the working set changed every way: updated (in place, HOT, and of an
-- indexed column), deleted, inserted, a keyless row's copies, a long value
UPDATE kp.t SET w = w || 'x' WHERE id % 10 = 0;
UPDATE kp.t SET v = v + 100 WHERE id % 13 = 0;
DELETE FROM kp.t WHERE id % 17 = 0;
INSERT INTO kp.t SELECT g, 1, 'new' || g FROM generate_series(6001, 6500) g;
DELETE FROM kp.k WHERE k = 3;
INSERT INTO kp.k SELECT 4, 'more' FROM generate_series(1, 30);
UPDATE kp.big SET big = repeat('changed', 2000) WHERE id <= 5;
DELETE FROM kp.big WHERE id = 40;
SELECT rows_digest() <> :'at_load' AS changed, clean();

-- reset hard to the head: the pages given back, no row written
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT key, value::bigint AS counted FROM jsonb_each_text(since(:'before'))
WHERE key IN ('apply_rows', 'apply_index_tuples')
UNION ALL
SELECT key, (value::bigint > 0)::int FROM jsonb_each_text(since(:'before'))
WHERE key = 'relations_restored' ORDER BY 1;
SELECT rows_digest() = :'at_load' AS as_loaded, clean(), indexes_valid();
-- found by each index as the rows are
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM kp.t WHERE v >= 100;
SELECT id FROM kp.t WHERE w = 'w10';
SELECT count(*) FROM kp.t WHERE id = 17;
RESET enable_seqscan;
RESET enable_bitmapscan;
-- (a relation unchanged since kept its own storage, and its one snapshot)
SELECT kept('kp.other') AS other;

-- rows deleted and found dead through an index, which marks their entries
-- dead in its buffers (a hint, its pages not copied for it): given back,
-- the index's entries are live again with their rows
DELETE FROM kp.t WHERE id BETWEEN 20 AND 29;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM kp.t WHERE id BETWEEN 20 AND 29;
SELECT count(*) FROM kp.t WHERE id BETWEEN 20 AND 29;
SELECT dolt_reset('--hard');
SELECT count(*) FROM kp.t WHERE id BETWEEN 20 AND 29;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT rows_digest() = :'at_load' AS as_loaded, indexes_valid();

-- what happens next is as after any reset: changes, a commit, VACUUM
UPDATE kp.t SET v = -1 WHERE id = 1;
VACUUM kp.t;
SELECT (dolt_commit('-am', 'after the reset'))[1] AS second \gset
SELECT clean(), indexes_valid();
SELECT rows_digest() AS at_second \gset

-- the commit before the head is kept too: reset hard to it, no row written
UPDATE kp.t SET v = 0 WHERE id <= 100;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT pg_stat_force_next_flush();
SELECT since(:'before') ->> 'apply_rows' AS applied,
	   (since(:'before') ->> 'relations_restored')::int > 0 AS restored;
SELECT rows_digest() = :'at_load' AS as_loaded, clean(), indexes_valid();

-- and back to the commit made after the reset: kept too (the last two
-- are), and though the reset just made gave kp.t new storage, its
-- snapshots of that commit were carried into it: no row written
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard', :'second');
SELECT pg_stat_force_next_flush();
SELECT since(:'before') ->> 'apply_rows' AS applied;
SELECT rows_digest() = :'at_second' AS as_second, clean(), indexes_valid();

-- a soft reset to the commit before, then a hard one: kept as of it still
UPDATE kp.k SET note = 'soft' WHERE k = 1;
SELECT dolt_reset('--soft', 'HEAD~1');
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT since(:'before') ->> 'apply_rows' AS applied;
SELECT rows_digest() = :'at_load' AS as_loaded, clean(), indexes_valid();

-- a table whose storage was replaced since (TRUNCATE) or that has an index
-- made since can't be given its pages: its rows are written, the others'
-- pages given back
TRUNCATE kp.k;
CREATE INDEX big_id ON kp.big (id, md5(big));
UPDATE kp.t SET v = 3 WHERE id <= 10;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT (since(:'before') ->> 'apply_rows')::int >= 1000 AS keyless_rows_written,
	   (since(:'before') ->> 'relations_restored')::int > 0 AS t_restored;
SELECT rows_digest() = :'at_load' AS as_loaded, clean(), indexes_valid();
SELECT count(*) FROM pg_indexes WHERE indexname = 'big_id';

-- a column added since: the pages given back, the column dropped by the move
ALTER TABLE kp.t ADD COLUMN extra int DEFAULT 5;
UPDATE kp.t SET extra = 6 WHERE id <= 10;
SELECT dolt_reset('--hard');
SELECT count(*) FROM information_schema.columns
WHERE table_schema = 'kp' AND table_name = 't' AND column_name = 'extra';
SELECT rows_digest() = :'at_load' AS as_loaded, clean(), indexes_valid();

-- a reset rolled back leaves the working set as it was, and the pages kept
UPDATE kp.t SET v = 9 WHERE id <= 10;
BEGIN;
SELECT dolt_reset('--hard');
SELECT rows_digest() = :'at_load' AS as_loaded;
ROLLBACK;
SELECT count(*) FROM kp.t WHERE v = 9;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT since(:'before') ->> 'apply_rows' AS applied;
SELECT rows_digest() = :'at_load' AS as_loaded, clean(), indexes_valid();

-- a commit rolled back to a savepoint keeps nothing (its snapshots let go
-- of); the pages are still kept as of the head
UPDATE kp.t SET v = 10 WHERE id <= 10;
BEGIN;
SAVEPOINT s;
SELECT dolt_commit('-am', 'undone') IS NOT NULL AS committed;
ROLLBACK TO s;
COMMIT;
SELECT dolt_reset('--hard');
SELECT kept('kp.t') <= 3 AS few_kept;
SELECT rows_digest() = :'at_load' AS as_loaded, clean();

-- however many commits, each relation keeps no more roots than kept
-- commits (a window of two), and those of the commits last retired (let
-- go of once the transaction retiring them is known to have committed)
ALTER DATABASE kept_pages SET prolly.history_min_commits = 2;
ALTER DATABASE kept_pages SET prolly.history_window = '1 microsecond';
UPDATE kp.t SET v = 1 WHERE id = 2;
SELECT dolt_commit('-am', 'step 1') IS NOT NULL AS committed;
UPDATE kp.t SET v = 2 WHERE id = 2;
SELECT dolt_commit('-am', 'step 2') IS NOT NULL AS committed;
UPDATE kp.t SET v = 3 WHERE id = 2;
SELECT dolt_commit('-am', 'step 3') IS NOT NULL AS committed;
UPDATE kp.t SET v = 4 WHERE id = 2;
SELECT dolt_commit('-am', 'step 4') IS NOT NULL AS committed;
UPDATE kp.t SET v = 5 WHERE id = 2;
SELECT dolt_commit('-am', 'step 5') IS NOT NULL AS committed;
UPDATE kp.t SET v = 6 WHERE id = 2;
SELECT dolt_commit('-am', 'step 6') IS NOT NULL AS committed;
SELECT test_retain_roots('main');
UPDATE kp.t SET v = 7 WHERE id = 2;
SELECT dolt_commit('-am', 'step 7') IS NOT NULL AS committed;
SELECT kept('kp.t') <= 3 AS t, kept('kp.t_v') <= 3 AS t_v;
ALTER DATABASE kept_pages RESET prolly.history_min_commits;
ALTER DATABASE kept_pages SET prolly.history_window = '1000 years';

-- with none kept, a reset writes the rows back
SET debug_prolly_page_roots = off;
UPDATE kp.t SET v = 11 WHERE id <= 10;
SELECT dolt_commit('-am', 'not kept') IS NOT NULL AS committed;
SELECT rows_digest() AS unkept \gset
UPDATE kp.t SET v = 12 WHERE id <= 10;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT (since(:'before') ->> 'apply_rows')::int AS applied,
	   since(:'before') ->> 'relations_restored' AS restored;
SELECT rows_digest() = :'unkept' AS as_committed, clean();
RESET debug_prolly_page_roots;

-- a branch renamed keeps its record; deleted, its record goes with it
SELECT dolt_branch('kb');
SELECT dolt_checkout('kb');
UPDATE kp.t SET v = 13 WHERE id <= 10;
SELECT dolt_reset('--hard');
SELECT dolt_checkout('main');
SELECT dolt_branch('-m', 'kb', 'kb2');
SELECT dolt_checkout('kb2');
UPDATE kp.t SET v = 14 WHERE id <= 10;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT since(:'before') ->> 'apply_rows' AS applied;
SELECT rows_digest() = :'unkept' AS as_committed, clean();
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'kb2');

-- an AFTER trigger's event queued in the transaction holds a row's
-- location in a table's storage: the table isn't given its pages, its rows
-- are written (and the event finds its row as the reset left it)
CREATE FUNCTION public.kp_seen() RETURNS trigger LANGUAGE plpgsql AS
$$BEGIN RETURN NULL; END$$;
CREATE CONSTRAINT TRIGGER t_deferred AFTER UPDATE ON kp.t
	DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION public.kp_seen();
SELECT dolt_commit('-Am', 'with a trigger') IS NOT NULL AS committed;
SELECT rows_digest() AS with_trigger \gset
DELETE FROM kp.t WHERE id BETWEEN 940 AND 960;
BEGIN;
UPDATE kp.t SET v = -v WHERE id = 970;
SELECT dolt_reset('--hard');
COMMIT;
SELECT rows_digest() = :'with_trigger' AS as_committed, clean(), indexes_valid();
DROP TRIGGER t_deferred ON kp.t;
SELECT dolt_commit('-am', 'without the trigger') IS NOT NULL AS committed;

-- a commit in a transaction rolled back by a session that then exits: the
-- snapshots it took are let go of as the session exits (of a table no
-- retired commit keeps, so only the rolled-back commit's could be left)
CREATE TABLE kp.fresh (id int PRIMARY KEY, v int);
INSERT INTO kp.fresh SELECT generate_series(1, 10), 0;
SELECT dolt_commit('-Am', 'fresh') IS NOT NULL AS committed;
SELECT kept('kp.fresh') AS kept_before;
\c kept_pages
SELECT pg_backend_pid() AS gone \gset
BEGIN;
UPDATE kp.fresh SET v = 15;
SELECT dolt_commit('-am', 'rolled back') IS NOT NULL AS committed;
ROLLBACK;
\c kept_pages
CREATE FUNCTION pg_temp.wait_gone(pid int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	WHILE EXISTS (SELECT FROM pg_stat_activity a WHERE a.pid = wait_gone.pid) LOOP
		PERFORM pg_sleep(0.01);
	END LOOP;
END $$;
SELECT pg_temp.wait_gone(:gone);
SELECT kept('kp.fresh') AS kept_after;

-- a commit amended away is no longer the branch's: its pages are let go
-- of, so the commit before it stays kept, and a reset to it writes no row
SELECT dolt_reset('--hard') IS NOT NULL AS clean;
SELECT rows_digest() AS before_amend \gset
UPDATE kp.t SET v = 3 WHERE id <= 50;
SELECT dolt_commit('-am', 'to amend') IS NOT NULL AS committed;
UPDATE kp.t SET v = 4 WHERE id <= 50;
SELECT dolt_commit('--amend') IS NOT NULL AS amended;
UPDATE kp.t SET v = 5 WHERE id <= 50;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT pg_stat_force_next_flush();
SELECT since(:'before') ->> 'apply_rows' AS applied,
	   (since(:'before') ->> 'relations_restored')::int > 0 AS restored;
SELECT rows_digest() = :'before_amend' AS as_before, clean(), indexes_valid();

-- a few rows changed since a baseline holding the commit's: written back,
-- not restored (prolly.restore_min_changes); as many as the threshold:
-- restored
SET prolly.restore_min_changes = 64;
SELECT dolt_commit('-Am', 'threshold', '--allow-empty') IS NOT NULL AS committed;
SELECT rows_digest() AS at_threshold \gset
UPDATE kp.t SET v = v + 1 WHERE id <= 3;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard') IS NOT NULL AS reset;
SELECT pg_stat_force_next_flush();
SELECT since(:'before') ->> 'relations_restored' AS restored,
	   since(:'before') ->> 'apply_rows' AS applied;
-- (the threshold asked of the log's entries alone: no row of the reset's
-- read as a fold reads them)
UPDATE kp.t SET v = v + 1 WHERE id <= 64;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT value AS candidates FROM pg_stat_prolly WHERE name = 'fold_candidates' \gset
SELECT dolt_reset('--hard') IS NOT NULL AS reset;
SELECT pg_stat_force_next_flush();
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   since(:'before') ->> 'apply_rows' AS applied,
	   (SELECT value FROM pg_stat_prolly WHERE name = 'fold_candidates') -
	   :candidates AS candidates;
SELECT rows_digest() = :'at_threshold' AS as_committed, clean(), indexes_valid();
RESET prolly.restore_min_changes;

-- every block the stores hold is held by a map or a snapshot kept
CHECKPOINT;
SELECT freed FROM prolly_page_sweep();

\c :home
DROP DATABASE kept_pages;
