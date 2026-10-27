-- The pages changed since a frozen root are found by the map's nodes set
-- since its epoch alone: a change stamps every pointer above its leaf, so
-- a commit's keep reads the nodes above its changed pages, and a restore's
-- check of a table unchanged since its root reads no node at all.

SELECT current_database() AS home \gset
CREATE DATABASE root_changed_pages;
ALTER DATABASE root_changed_pages SET prolly.restore_min_changes = 0;
\c root_changed_pages

CREATE FUNCTION nodes_read() RETURNS bigint LANGUAGE sql AS $$
	SELECT value FROM pg_stat_prolly WHERE name = 'bmap_changed_nodes_read'
$$;

-- a table of several of the map's leaves (each maps about 500 pages)
CREATE SCHEMA cp VERSIONED;
CREATE TABLE cp.t (id int PRIMARY KEY, w text NOT NULL)
	WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO cp.t SELECT g, repeat('w', 80) FROM generate_series(1, 30000) g;
SELECT pg_relation_size('cp.t') / 8192 > 2500 AS several_leaves;
SELECT dolt_commit('-Am', 'base') IS NOT NULL AS committed;

-- a one-row commit: the nodes above its page (and its change log's)
UPDATE cp.t SET w = 'changed' WHERE id = 29000;
SELECT pg_stat_force_next_flush();
SELECT nodes_read() AS before \gset
SELECT dolt_commit('-am', 'one row') IS NOT NULL AS committed;
SELECT pg_stat_force_next_flush();
SELECT nodes_read() - :before AS one_row_commit_nodes;

-- a reset to the head, the table unchanged since: no node read
SELECT nodes_read() AS before \gset
SELECT dolt_reset('--hard', 'HEAD') IS NOT NULL AS reset;
SELECT pg_stat_force_next_flush();
SELECT nodes_read() - :before AS unchanged_reset_nodes;

-- the commit's rows, frozen, after a move back to it
UPDATE cp.t SET w = 'later' WHERE id <= 10;
SELECT dolt_commit('-am', 'later') IS NOT NULL AS committed;
SELECT dolt_reset('--hard', 'HEAD~1') IS NOT NULL AS reset;
SELECT count(*) FILTER (WHERE w = 'changed') AS changed,
	count(*) FILTER (WHERE w = 'later') AS later FROM cp.t;

\c :home
DROP DATABASE root_changed_pages;
