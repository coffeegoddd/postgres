-- Large values in versioned tables: TOAST holds them in the working set;
-- a commit stores them out of line, in value trees addressed by byte
-- counts.  They read back whole from history, and a few bytes inserted
-- in the middle of one add a few nodes to the store, not one per chunk
-- after them.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA lv VERSIONED;
CREATE TABLE lv.big (id int PRIMARY KEY, t text, b bytea);
INSERT INTO lv.big
SELECT g, string_agg(md5(g || ':' || i), '' ORDER BY i),
	decode(repeat(to_hex(g * 17), 30000), 'hex')
FROM generate_series(1, 5) g, generate_series(1, 2000 * g) i GROUP BY g;
CREATE TABLE lv.keyless (t text);
INSERT INTO lv.keyless SELECT repeat('k', 100000) FROM generate_series(1, 3);
CREATE VIEW lv.digest AS
SELECT string_agg(id || ':' || length(t) || ':' || md5(t) || ':' || md5(b), ','
				  ORDER BY id) AS d
FROM lv.big;
SELECT d AS before FROM lv.digest \gset
SELECT left(:'before', 9) AS first_row;
SELECT (dolt_commit('-Am', 'large values'))[1] AS c1 \gset

-- Three bytes inserted into the middle of a 192 KB value.
UPDATE lv.big SET t = overlay(t PLACING 'XYZ' FROM 96000 FOR 0) WHERE id = 3;
SELECT length(t) FROM lv.big WHERE id = 3;
SELECT d AS after FROM lv.digest \gset
SELECT (dolt_commit('-am', 'an insertion'))[1] AS c2 \gset
SELECT test_walk_new_count(:'c1', :'c2') < 20 AS few_nodes_added;
SELECT diff_type, (from_row).id, length((from_row).t), length((to_row).t)
FROM dolt_diff(:'c1', :'c2', NULL::lv.big);

-- Back to the first commit and forward again, from history alone.
SELECT dolt_reset('--hard', :'c1');
SELECT d = :'before' AS restored FROM lv.digest;
DELETE FROM lv.keyless;
SELECT dolt_reset('--hard', :'c2');
SELECT d = :'after' AS forward FROM lv.digest;
SELECT count(*), sum(length(t)) FROM lv.keyless;

DROP SCHEMA lv CASCADE;
SELECT length(dolt_commit('-Am', 'lv gone')::text) AS committed;
