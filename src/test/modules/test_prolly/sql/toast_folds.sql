-- A fold makes the same tree of the same rows however their values are
-- stored: by content, or in a TOAST relation compressed or not; moved
-- out of line, or inline.  (A value stored by content whose
-- tree holds the column's canonical bytes is that tree: the fold writes
-- its reference without reading the value; 071_toast_folds.pl.)

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA tf VERSIONED;
CREATE TABLE tf.rows (id int PRIMARY KEY, t text, b bytea, j jsonb);
CREATE VIEW tf.source AS
SELECT g AS id, string_agg(md5(g || '/' || i), '' ORDER BY i) AS t,
	   decode(string_agg(md5(g || '|' || i), '' ORDER BY i), 'hex') AS b,
	   jsonb_build_object('g', g, 'parts', jsonb_agg(md5(g || '-' || i))) AS j
FROM generate_series(1, 3) g, generate_series(1, 3000) i GROUP BY g;

-- by content
CREATE TABLE tf.by_content (LIKE tf.rows INCLUDING ALL);
INSERT INTO tf.by_content SELECT * FROM tf.source;
SELECT count(*) FILTER (WHERE test_toast_pointer(t) IS NOT NULL) AS by_content
FROM tf.by_content;

-- in the TOAST relation, compressed and not
SET debug_toast_relation = on;
CREATE TABLE tf.toasted (LIKE tf.rows INCLUDING ALL);
INSERT INTO tf.toasted SELECT * FROM tf.source;
CREATE TABLE tf.toasted_plain (LIKE tf.rows INCLUDING ALL);
ALTER TABLE tf.toasted_plain ALTER COLUMN t SET STORAGE EXTERNAL,
	ALTER COLUMN j SET STORAGE EXTERNAL;
INSERT INTO tf.toasted_plain SELECT * FROM tf.source;
RESET debug_toast_relation;
SELECT count(*) FILTER (WHERE pg_column_toast_chunk_id(t) IS NOT NULL) AS in_toast
FROM tf.toasted;

-- One tree for the same rows.
SELECT count(DISTINCT tree) AS trees FROM test_fold_trees()
WHERE name IN ('tf.by_content', 'tf.toasted', 'tf.toasted_plain');

-- A value short enough to be inline in history, moved out by content
-- because its row was long, is inline in history as when stored inline.
CREATE TABLE tf.short_out (id int PRIMARY KEY, a text, b text, c text, d text);
CREATE TABLE tf.short_in (id int PRIMARY KEY, a text, b text, c text, d text);
ALTER TABLE tf.short_in ALTER COLUMN a SET STORAGE MAIN, ALTER COLUMN b SET STORAGE MAIN,
	ALTER COLUMN c SET STORAGE MAIN, ALTER COLUMN d SET STORAGE MAIN;
CREATE VIEW tf.short AS
SELECT 1 AS id, string_agg(md5(i::text), '') FILTER (WHERE i <= 60) AS a,
	   string_agg(md5(i::text), '') FILTER (WHERE i > 60) AS b,
	   string_agg(md5(i::text || 'c'), '') AS c, string_agg(md5(i::text || 'd'), '') AS d
FROM generate_series(1, 120) i;
INSERT INTO tf.short_out SELECT * FROM tf.short;
INSERT INTO tf.short_in SELECT * FROM tf.short;
SELECT count(*) FILTER (WHERE test_toast_pointer(v) IS NOT NULL) AS moved_out
FROM tf.short_out, LATERAL (VALUES (a), (b), (c), (d)) x(v);
SELECT count(*) FILTER (WHERE test_toast_pointer(v) IS NOT NULL) AS moved_out
FROM tf.short_in, LATERAL (VALUES (a), (b), (c), (d)) x(v);
SELECT count(DISTINCT tree) AS trees FROM test_fold_trees()
WHERE name IN ('tf.short_out', 'tf.short_in');

-- Committed and reset, the rows come back.
SELECT md5(string_agg(t || b::text || j::text, ',' ORDER BY id)) AS rows
FROM tf.by_content \gset
SELECT length(dolt_commit('-Am', 'folds')::text) AS committed;
DELETE FROM tf.by_content;
SELECT dolt_reset('--hard') IS NOT NULL AS reset;
SELECT md5(string_agg(t || b::text || j::text, ',' ORDER BY id)) = :'rows' AS same
FROM tf.by_content;

DROP SCHEMA tf CASCADE;
