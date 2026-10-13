--
-- Canonical form: a commit's root hash depends only on its logical content
-- (schema and data), never on how it was produced
-- (specs/09-versioned-catalogs.md).  Two branches reach the same tables by
-- different histories: different statement orders, rows inserted and
-- deleted, columns added and dropped, a column added with a default, and
-- objects created and dropped first so that OIDs differ.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

SELECT 'commit start', prolly_commit('vh start') IS NOT NULL;
SELECT 'branch a', prolly_branch_create('vh_a', NULL) IS NULL;
SELECT 'branch b', prolly_branch_create('vh_b', NULL) IS NULL;
SELECT 'branch c', prolly_branch_create('vh_c', NULL) IS NULL;

-- History a: the long way round.
SELECT 'checkout a', prolly_checkout('vh_a') IS NULL;
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
SELECT prolly_commit('vh a') AS vh_a_commit \gset

-- History b: straight to the result, in another order.
SELECT 'checkout b', prolly_checkout('vh_b') IS NULL;
CREATE TABLE vh2 (a text PRIMARY KEY);
INSERT INTO vh2 VALUES ('y'), ('x');
CREATE TABLE vh (id int PRIMARY KEY, v text, c int DEFAULT 5);
CREATE INDEX vh_v ON vh (v);
INSERT INTO vh SELECT g, 'v' || g, 5 FROM generate_series(199, 1, -2) g;
SELECT prolly_commit('vh b') AS vh_b_commit \gset

-- History c: like b, but one value differs.
SELECT 'checkout c', prolly_checkout('vh_c') IS NULL;
CREATE TABLE vh2 (a text PRIMARY KEY);
INSERT INTO vh2 VALUES ('y'), ('x');
CREATE TABLE vh (id int PRIMARY KEY, v text, c int DEFAULT 5);
CREATE INDEX vh_v ON vh (v);
INSERT INTO vh SELECT g, 'v' || g, CASE WHEN g = 99 THEN 6 ELSE 5 END
FROM generate_series(199, 1, -2) g;
SELECT prolly_commit('vh c') AS vh_c_commit \gset

SELECT 'a and b: same content', (SELECT count(*) FROM vh) = 100;
SELECT 'a and b: same root', prolly_root_hash('vh_a') = prolly_root_hash('vh_b');
SELECT 'a and b: different commits', :'vh_a_commit' <> :'vh_b_commit';
SELECT 'c: different root', prolly_root_hash('vh_c') <> prolly_root_hash('vh_b');
SELECT 'root by commit id', prolly_root_hash(:'vh_a_commit') = prolly_root_hash('vh_a');

-- The same commit's root hash is stable.
SELECT 'stable', prolly_root_hash('vh_a') = prolly_root_hash('vh_a');
SELECT 'back to main', prolly_checkout('main') IS NULL;
