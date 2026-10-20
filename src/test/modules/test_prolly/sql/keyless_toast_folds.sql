-- A keyless table's rows with values out of line are logged with their
-- hash pointers (by content in the chunk store), so a fold reads only
-- what changed, not the table in full; the same tree as a full fold.
CREATE DATABASE regress_keyless_toast;
\c regress_keyless_toast
CREATE EXTENSION test_prolly;
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE FUNCTION full_reads() RETURNS bigint LANGUAGE sql AS $$
	SELECT value FROM pg_stat_prolly WHERE name = 'fold_full_tables'
$$;
CREATE FUNCTION same_as_full() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	inc text;
BEGIN
	inc := test_fold_root();
	RETURN inc = test_fold_root(true);
END $$;
-- (values too long to be inline, not compressible: out of line, by hash)
CREATE FUNCTION big(seed int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT string_agg(md5((seed * 10000 + g)::text), '') FROM generate_series(1, 3200) g
$$;
CREATE SCHEMA kt VERSIONED;
CREATE TABLE kt.t (a int, b text STORAGE EXTERNAL);
CREATE TABLE kt.j (a int, doc jsonb STORAGE EXTERNAL, raw bytea STORAGE EXTERNAL);
INSERT INTO kt.t SELECT g, big(g) FROM generate_series(1, 6) g;
INSERT INTO kt.t SELECT 7, big(7) FROM generate_series(1, 3);
INSERT INTO kt.t SELECT g, 'small' FROM generate_series(1, 50) g;
INSERT INTO kt.j SELECT g, jsonb_build_object('v', big(g)), convert_to(big(g + 50), 'UTF8')
FROM generate_series(1, 4) g;
SELECT length(dolt_commit('-Am', 'loaded')::text) AS committed;

-- updated, deleted, inserted rows with long values; one copy of a
-- duplicate deleted: from the baseline
UPDATE kt.t SET b = big(100) WHERE a = 1 AND b <> 'small';
DELETE FROM kt.t WHERE a = 2 AND b <> 'small';
INSERT INTO kt.t VALUES (8, big(8));
DELETE FROM kt.t WHERE ctid = (SELECT min(ctid) FROM kt.t WHERE a = 7);
UPDATE kt.j SET doc = jsonb_build_object('v', big(200)) WHERE a = 1;
DELETE FROM kt.j WHERE a = 2;
INSERT INTO kt.j VALUES (5, jsonb_build_object('v', big(5)), convert_to(big(55), 'UTF8'));
SELECT same_as_full() AS same;
:reset
SELECT length(dolt_commit('-am', 'changed')::text) AS committed;
:flush
SELECT full_reads() AS read_in_full;
SELECT count(*) AS copies_of_7 FROM kt.t WHERE a = 7;
SELECT diff_type, count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::kt.t) GROUP BY 1 ORDER BY 1;
SELECT diff_type, count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::kt.j) GROUP BY 1 ORDER BY 1;

-- a row too long for an entry's page, stored inline: still read in full
CREATE TABLE kt.wide (a int, b text STORAGE PLAIN);
SELECT length(dolt_commit('-Am', 'wide')::text) AS committed;
INSERT INTO kt.wide VALUES (1, repeat('w', 8120));
SELECT same_as_full() AS same;
:reset
SELECT length(dolt_commit('-am', 'wide row')::text) AS committed;
:flush
SELECT full_reads() AS read_in_full;

\c contrib_regression
DROP DATABASE regress_keyless_toast;
