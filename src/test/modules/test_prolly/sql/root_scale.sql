-- Roots at scale: a page store keeping thousands of captured roots.  What
-- each operation on them costs is of what it changes, or of the roots it
-- lets go of, never of every root the store keeps, nor of the table: a
-- capture, a release, and its drain; VACUUM's settling of the roots (only
-- the pages of slots marked as naming a transaction); a sweep's lowering of
-- each relation's shared_below (the roots read once, not once a relation);
-- VACUUM of a table every root holds, copying only the pages it changes.

SELECT current_database() AS home \gset
CREATE DATABASE root_scale;
\c root_scale
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET debug_block_mapped_heap = on;

-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked, the session's counts flushed first (:flush, a statement of its
-- own)
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
CREATE FUNCTION mark() RETURNS void LANGUAGE sql AS $$
	SELECT since(c) FROM unnest('{bmap_roots_pages_read,bmap_release_drained,bmap_release_refcount_updates,bmap_release_blocks_freed,page_copies_shared}'::text[]) c
$$;

-- a table of 6667 pages, and 1500 roots of it, each captured after one
-- row is changed: each root's own pages are a few, the rest shared
CREATE TABLE big (a int, b text) WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO big SELECT g, repeat('b', 200) FROM generate_series(1, 20000) g;
SELECT pg_relation_filenode('big') AS big \gset
CREATE TABLE ids (n int PRIMARY KEY, id int8);
DO $$
BEGIN
	FOR i IN 1..1500 LOOP
		UPDATE big SET b = 'x' || i WHERE a = (i * 7919) % 20000 + 1;
		INSERT INTO ids VALUES (i, test_bmap_root_capture(pg_relation_filenode('big')));
		COMMIT;
	END LOOP;
END $$;
SELECT held, slots / 1000 AS slots_k FROM test_bmap_roots();
SELECT pg_relation_size('big') / 8192 AS big_pages;
-- (every root settled once, as VACUUM does before the database's oldest
-- transaction id advances)
VACUUM ids;

-- a capture reads one page of slots, whatever the roots
:flush
SELECT mark();
SELECT test_bmap_root_capture(:big) IS NOT NULL AS captured;
:flush
SELECT since('bmap_roots_pages_read') AS capture_pages_read;

-- a release reads one, and its drain lets go of what the root alone held:
-- the blocks of its page changed and of the tree's path to it, a few
SELECT mark();
SELECT test_bmap_root_release(id) FROM ids WHERE n = 750;
:flush
SELECT since('bmap_roots_pages_read') AS release_pages_read;
SELECT mark();
SELECT test_bmap_release_drain(1000000) IS NOT NULL AS drained;
:flush
SELECT since('bmap_release_drained') BETWEEN 1 AND 20 AS drain_entries_few,
	since('bmap_release_refcount_updates') + since('bmap_release_blocks_freed')
		BETWEEN 1 AND 20 AS drain_work_few;

-- a hundred released: the drain's work a hundred times one's, at most
SELECT mark();
SELECT count(test_bmap_root_release(id)) AS released FROM ids WHERE n BETWEEN 1 AND 100;
:flush
SELECT since('bmap_roots_pages_read') <= 2 * 100 AS release_pages_read_each;
SELECT mark();
SELECT test_bmap_release_drain(1000000) IS NOT NULL AS drained;
:flush
SELECT since('bmap_release_drained') BETWEEN 100 AND 2000 AS drain_entries_per_root;

-- VACUUM settles only the pages of slots marked: those of the roots let
-- go of since, not every page of the 1500 roots' slots
SELECT mark();
VACUUM ids;
:flush
SELECT since('bmap_roots_pages_read') AS settle_pages_read_released;
SELECT mark();
VACUUM ids;
:flush
SELECT since('bmap_roots_pages_read') AS settle_pages_read_again;
-- a root captured, then settled: its page alone read again
SELECT test_bmap_root_capture(:big) IS NOT NULL AS captured;
SELECT mark();
VACUUM ids;
:flush
SELECT since('bmap_roots_pages_read') <= 3 AS settle_pages_read_captured;
-- a capture rolled back: reclaimed by the settling, reading its page
BEGIN;
SELECT test_bmap_root_capture(:big) AS aborted \gset
ROLLBACK;
SELECT mark();
VACUUM ids;
:flush
SELECT since('bmap_roots_pages_read') <= 3 AS reclaim_pages_read;
SELECT test_bmap_root_held(:aborted) IS NULL AS aborted_not_held;
SELECT held FROM test_bmap_roots();

-- VACUUM of the table every root holds: it copies only the pages it
-- changes (those with dead rows), not every page the roots share
SELECT test_bmap_root_capture(:big) AS before_vacuum \gset
SELECT test_bmap_rows_digest(:big) AS before_vacuum_rows \gset
DELETE FROM big WHERE a % 1000 = 0;
SELECT mark();
VACUUM big;
:flush
SELECT since('page_copies_shared') BETWEEN 1 AND 40 AS vacuum_copies_changed;

-- a sweep: what every root holds counted once; the roots read once for
-- the relations' shared_below too, not once a relation (fifty here)
DO $$
BEGIN
	FOR i IN 1..50 LOOP
		EXECUTE format('CREATE TABLE r%s (a int) WITH (autovacuum_enabled = off)', i);
		EXECUTE format('INSERT INTO r%s VALUES (1)', i);
	END LOOP;
END $$;
SELECT (slots / 68) AS slot_pages FROM test_bmap_roots() \gset
SELECT mark();
SELECT freed >= 0 AS swept FROM prolly_page_sweep();
:flush
SELECT since('bmap_roots_pages_read') <= 3 * :slot_pages + 10 AS sweep_pages_read;
-- most roots let go of, drained: a sweep frees what they alone held and
-- shortens the store, moving blocks the others hold (compaction); the
-- roots kept still have their pages as captured
SELECT count(test_bmap_root_release(id)) AS released FROM ids WHERE n BETWEEN 101 AND 1400;
SELECT mark();
SELECT test_bmap_release_drain(100000000) IS NOT NULL AS drained;
:flush
SELECT since('bmap_release_blocks_freed') > 0 AS drain_freed;
SELECT truncated > 0 AS shortened, moved > 0 AS moved FROM prolly_page_sweep();
SELECT held FROM test_bmap_roots();
SELECT test_bmap_share_root(:before_vacuum) AS copy \gset
SELECT test_bmap_rows_digest(:copy) = :'before_vacuum_rows' AS before_vacuum_as_captured;
SELECT test_bmap_unlink(:copy);
SELECT test_bmap_share_root(id) AS copy FROM ids WHERE n = 1500 \gset
SELECT count(*) = test_bmap_root_held(:'before_vacuum') AS last_root_whole
FROM test_bmap_map(:copy);
SELECT test_bmap_unlink(:copy);

\c :home
DROP DATABASE root_scale;
