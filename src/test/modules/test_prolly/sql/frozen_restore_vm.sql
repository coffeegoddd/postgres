-- A restore of a frozen root, the relation changed since, gives the
-- restored relation a visibility map: the source's for the pages
-- unchanged since, cleared for each page changed since (its frozen copy
-- not read).  An index-only scan after the restore fetches the changed
-- pages from the heap until VACUUM sets their bits again, and the map is
-- true of the pages throughout.

SELECT current_database() AS home \gset
CREATE DATABASE frozen_restore_vm;
\c frozen_restore_vm
SET client_min_messages = warning;
CREATE EXTENSION pg_visibility;
RESET client_min_messages;

CREATE SCHEMA fv VERSIONED;
CREATE TABLE fv.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL)
	WITH (autovacuum_enabled = off);
INSERT INTO fv.t SELECT g, 0, repeat('w', 40) FROM generate_series(1, 5000) g;
VACUUM fv.t;
SELECT dolt_commit('-Am', 'c1') IS NOT NULL AS committed;

-- every page changed since c1's root, then a reset to it
UPDATE fv.t SET v = 1;
SELECT dolt_commit('-am', 'c2') IS NOT NULL AS committed;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT count(*) FILTER (WHERE v = 0) AS at_c1 FROM fv.t;

-- the map: every page changed, so none all-visible or all-frozen
SELECT all_visible, all_frozen FROM pg_visibility_map_summary('fv.t');
SELECT count(*) AS not_frozen FROM pg_check_frozen('fv.t');
SELECT count(*) AS not_visible FROM pg_check_visible('fv.t');

-- an index-only scan fetches each row from the heap.  The pages are the
-- root's still, shared with it, so a VACUUM leaves them as they are, but
-- sets their bits again (they are all-visible and frozen)
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(id) FROM fv.t WHERE id <= 5000;
VACUUM fv.t;
SELECT all_visible = pg_relation_size('fv.t') / 8192 AS all_visible,
	all_frozen = pg_relation_size('fv.t') / 8192 AS all_frozen
FROM pg_visibility_map_summary('fv.t');
SELECT count(*) AS not_frozen FROM pg_check_frozen('fv.t');
SELECT count(*) AS not_visible FROM pg_check_visible('fv.t');
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(id) FROM fv.t WHERE id <= 5000;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- a root with dead line pointers left (rows updated, not vacuumed, before
-- the commit), every page changed since: none all-visible; an index-only
-- scan finds only the commit's rows
UPDATE fv.t SET v = 2 WHERE id <= 100;
SELECT dolt_commit('-am', 'c3') IS NOT NULL AS committed;
UPDATE fv.t SET v = 3;
SELECT dolt_commit('-am', 'c4') IS NOT NULL AS committed;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT count(*) FILTER (WHERE v = 2) AS updated, count(*) AS rows FROM fv.t;
SELECT all_visible, all_frozen FROM pg_visibility_map_summary('fv.t');
SELECT count(*) AS not_frozen FROM pg_check_frozen('fv.t');
SELECT count(*) AS not_visible FROM pg_check_visible('fv.t');
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(id) AS by_index FROM fv.t WHERE id <= 5000;
RESET enable_seqscan;
RESET enable_bitmapscan;

\c :home
DROP DATABASE frozen_restore_vm;
