--
-- Canonical form: a commit's root hash depends only on its logical content
-- (schema and data), never on how it was produced.  Two branches reach the
-- same tables by
-- different histories: different statement orders, rows inserted and
-- deleted, columns added and dropped, a column added with a default, and
-- objects created and dropped first so that OIDs differ.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

\getenv abs_srcdir PG_ABS_SRCDIR
\set vc_setup :abs_srcdir '/sql/vc_versioned_public.sql'
\i :vc_setup

SELECT 'commit start', length((dolt_commit('-Am', 'vh start', '--allow-empty'))[1]) = 64;
SELECT 'branch a', (dolt_branch('vh_a'))[1] = '0';
SELECT 'branch b', (dolt_branch('vh_b'))[1] = '0';
SELECT 'branch c', (dolt_branch('vh_c'))[1] = '0';

-- History a: the long way round.
SELECT 'checkout a', (dolt_checkout('vh_a'))[1] = '0';
CREATE TABLE vh_junk (x int);
CREATE TYPE vh_junk_type AS (a int);
DROP TABLE vh_junk;
DROP TYPE vh_junk_type;
CREATE TABLE vh (id int PRIMARY KEY, v text);
INSERT INTO vh SELECT g, 'tmp' FROM generate_series(1, 200) g;
DELETE FROM vh WHERE id % 2 = 0;
UPDATE vh SET v = 'v' || id;
ALTER TABLE vh ADD COLUMN w int;
UPDATE vh SET w = id;
ALTER TABLE vh DROP COLUMN w;
ALTER TABLE vh ADD COLUMN c int DEFAULT 5;
CREATE INDEX vh_v ON vh (v);
CREATE TABLE vh2 (a text PRIMARY KEY);
INSERT INTO vh2 VALUES ('x'), ('y'), ('z');
DELETE FROM vh2 WHERE a = 'z';
SELECT 'a: content', (SELECT count(*) FROM vh) = 100 AND (SELECT count(*) FROM vh2) = 2;
SELECT (dolt_commit('-Am', 'vh a'))[1] AS vh_a_commit \gset

-- History b: straight to the result, in another order.
SELECT 'checkout b', (dolt_checkout('vh_b'))[1] = '0';
CREATE TABLE vh2 (a text PRIMARY KEY);
INSERT INTO vh2 VALUES ('y'), ('x');
CREATE TABLE vh (id int PRIMARY KEY, v text, c int DEFAULT 5);
CREATE INDEX vh_v ON vh (v);
INSERT INTO vh SELECT g, 'v' || g, 5 FROM generate_series(199, 1, -2) g;
SELECT 'b: content', (SELECT count(*) FROM vh) = 100 AND (SELECT count(*) FROM vh2) = 2;
SELECT (dolt_commit('-Am', 'vh b'))[1] AS vh_b_commit \gset

-- History c: like b, but one value differs.
SELECT 'checkout c', (dolt_checkout('vh_c'))[1] = '0';
CREATE TABLE vh2 (a text PRIMARY KEY);
INSERT INTO vh2 VALUES ('y'), ('x');
CREATE TABLE vh (id int PRIMARY KEY, v text, c int DEFAULT 5);
CREATE INDEX vh_v ON vh (v);
INSERT INTO vh SELECT g, 'v' || g, CASE WHEN g = 99 THEN 6 ELSE 5 END
FROM generate_series(199, 1, -2) g;
SELECT 'c: content', (SELECT count(*) FROM vh) = 100 AND (SELECT count(*) FROM vh2) = 2;
SELECT (dolt_commit('-Am', 'vh c'))[1] AS vh_c_commit \gset

SELECT 'a and b: same root', prolly_root_hash('vh_a') = prolly_root_hash('vh_b');
SELECT 'a and b: different commits', :'vh_a_commit' <> :'vh_b_commit';
SELECT 'c: different root', prolly_root_hash('vh_c') <> prolly_root_hash('vh_b');
SELECT 'root by commit id', prolly_root_hash(:'vh_a_commit') = prolly_root_hash('vh_a');

-- The same commit's root hash is stable.
SELECT 'stable', prolly_root_hash('vh_a') = prolly_root_hash('vh_a');
SELECT 'back to main', (dolt_checkout('main'))[1] = '0';
