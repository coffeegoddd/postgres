-- A capture logs each page of the relation's that changed since the last
-- one as an image, and stamps those unchanged since with its record's
-- position (bmap_pages_stamped).  Each one's first change after copies it
-- to a new block; a page so stamped, captured after the redo pointer, is
-- copied without its image (bmap_copies_imageless): redo copies the old
-- block, which the capture's image wrote.  A checkpoint between the
-- capture and the change, or the "always" policy, logs every copy's image.

CREATE DATABASE cow_images;
\c cow_images
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE FUNCTION copies() RETURNS TABLE (stamped bool, imageless bool,
	shared bool) LANGUAGE sql AS $$
	SELECT max(value) FILTER (WHERE name = 'bmap_pages_stamped') > 0,
		max(value) FILTER (WHERE name = 'bmap_copies_imageless') > 0,
		max(value) FILTER (WHERE name = 'page_copies_shared') > 0
	FROM pg_stat_prolly
$$;
CREATE SCHEMA s VERSIONED;
CREATE TABLE s.t (id int PRIMARY KEY, v int NOT NULL, pad text NOT NULL);
CREATE INDEX t_v ON s.t (v);
INSERT INTO s.t SELECT i, i, repeat('x', 200) FROM generate_series(1, 5000) i;
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;

-- Rows spread over the table, their indexed column changed: the index's
-- pages, captured as they are, are copied as each first changes.
UPDATE s.t SET v = -v WHERE id % 50 = 0;
:reset
SELECT length(dolt_commit('-am', 'one')::text) > 0 AS committed;
:flush
SELECT stamped FROM copies();
:reset
UPDATE s.t SET v = v - 1 WHERE id % 50 = 0;
:flush
SELECT imageless, shared FROM copies();
SELECT count(*), sum(v) FROM s.t;

-- After a checkpoint, the capture is before the redo pointer.
SELECT length(dolt_commit('-am', 'two')::text) > 0 AS committed;
CHECKPOINT;
:reset
UPDATE s.t SET v = v - 1 WHERE id % 50 = 0;
:flush
SELECT imageless, shared FROM copies();

-- The "always" policy.
SELECT length(dolt_commit('-am', 'three')::text) > 0 AS committed;
SET debug_prolly_cow_image = 'always';
:reset
UPDATE s.t SET v = v - 1 WHERE id % 50 = 0;
:flush
SELECT imageless, shared FROM copies();
RESET debug_prolly_cow_image;
SET debug_prolly_cow_image = 'none such';
SELECT count(*), sum(v) FROM s.t;

\c contrib_regression
DROP DATABASE cow_images;
