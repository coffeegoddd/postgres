-- dolt_status and dolt_diff of branches made by copying: versions compared
-- as their folded trees.

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
CREATE TABLE app.gone (x int);
CREATE TABLE app.k (a text);
INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 2000) g;
INSERT INTO app.k VALUES ('x'), ('y'), ('y');
INSERT INTO app.gone VALUES (1);
SELECT length(dolt_commit('-m', 'first')::text) AS committed;
SELECT * FROM dolt_status();

-- Rows changed, a table made, one dropped, one redefined.
UPDATE app.t SET v = 'changed' WHERE id IN (7, 1500);
DELETE FROM app.t WHERE id = 3;
INSERT INTO app.t VALUES (2001, 'new');
CREATE TABLE app.made (y int);
DROP TABLE app.gone;
ALTER TABLE app.k ADD COLUMN b int;
SELECT * FROM dolt_status();
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::app.t);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'STAGED', NULL::app.t);

-- Between commits, either way round.
SELECT length(dolt_commit('-m', 'second')::text) AS committed;
SELECT * FROM dolt_status();
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~1', 'HEAD', NULL::app.t);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'HEAD~1', NULL::app.t);
SELECT count(*) FROM dolt_diff('HEAD', 'HEAD', NULL::app.t);

-- A keyless table's copies: each one added or removed is a row.
ALTER TABLE app.k DROP COLUMN b;
DELETE FROM app.k WHERE a = 'y';
INSERT INTO app.k VALUES ('z'), ('z');
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::app.k) ORDER BY 1, 2, 3;
SELECT * FROM dolt_status();

-- Another branch's head.
SELECT dolt_branch('f');
SELECT dolt_checkout('f');
UPDATE app.t SET v = 'on f' WHERE id = 1;
SELECT length(dolt_commit('-m', 'on f', '-a')::text) AS committed;
SELECT dolt_checkout('main');
SELECT diff_type, from_row, to_row FROM dolt_diff('main', 'f', NULL::app.t);

-- Bad refs and tables.
SELECT * FROM dolt_diff('nope', 'HEAD', NULL::app.t);
CREATE TABLE plain (x int);
SELECT * FROM dolt_diff('HEAD~1', 'HEAD', NULL::plain);
DROP TABLE plain;

SELECT dolt_branch('-D', 'f');
DROP SCHEMA app CASCADE;

-- A column added with a default: a row's NULL in it is NULL in the diff,
-- not the default (rows are folded, and read back, as a rewrite has them).
CREATE SCHEMA sdd VERSIONED;
CREATE TABLE sdd.t (id int PRIMARY KEY, a text);
INSERT INTO sdd.t VALUES (1, 'one'), (2, 'two');
SELECT length(dolt_commit('-Am', 'before the column')::text) AS committed;
ALTER TABLE sdd.t ADD COLUMN c int DEFAULT 5;
UPDATE sdd.t SET c = NULL WHERE id = 1;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::sdd.t)
  ORDER BY to_row;
DROP SCHEMA sdd CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;

-- Names in a database whose encoding isn't UTF-8 (the canonical tree's).
CREATE DATABASE sd_latin1 ENCODING 'LATIN1' LC_COLLATE 'C' LC_CTYPE 'C'
  TEMPLATE template0;
\c sd_latin1
SET client_encoding = 'UTF8';
CREATE SCHEMA "appé" VERSIONED;
CREATE TABLE "appé"."tablé" (id int PRIMARY KEY, v text);
INSERT INTO "appé"."tablé" VALUES (1, 'a');
SELECT length(dolt_commit('-Am', 'first')::text) AS committed;
INSERT INTO "appé"."tablé" VALUES (2, 'b');
SELECT * FROM dolt_status;
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::"appé"."tablé");
\c contrib_regression
DROP DATABASE sd_latin1;

-- One whose encoding orders names otherwise than UTF-8 does ("я" before
-- "€" in UTF-8, after it in WIN1251): only the table dropped is.
CREATE DATABASE sd_win1251 ENCODING 'WIN1251' LC_COLLATE 'C' LC_CTYPE 'C'
  TEMPLATE template0;
\c sd_win1251
SET client_encoding = 'UTF8';
CREATE SCHEMA app VERSIONED;
CREATE TABLE app."я" (id int PRIMARY KEY);
CREATE TABLE app."€" (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'two tables')::text) AS committed;
DROP TABLE app."я";
SELECT * FROM dolt_status ORDER BY table_name;
\c contrib_regression
DROP DATABASE sd_win1251;

-- Reading what changed doesn't need what a commit does: objects the
-- canonical form doesn't describe yet (a partitioned table, an aggregate,
-- a policy) are left out of status and diff, and only a commit refuses
-- them.
CREATE SCHEMA sdu VERSIONED;
CREATE TABLE sdu.t (id int PRIMARY KEY, v text);
SELECT length(dolt_commit('-Am', 'sdu')::text) AS committed;
-- They are changes a commit can't hold: listed as not covered (a
-- partition with its partitioned table, not as a table of its own), and
-- the branch is dirty with them alone.
CREATE TABLE sdu.p (id int) PARTITION BY RANGE (id);
CREATE TABLE sdu.p1 PARTITION OF sdu.p FOR VALUES FROM (1) TO (10);
SELECT name, dirty FROM dolt_branches WHERE name = 'main';
SELECT * FROM dolt_status WHERE table_name LIKE 'sdu.%';
CREATE AGGREGATE sdu.total (int) (sfunc = int4pl, stype = int);
CREATE POLICY sdu_own ON sdu.t USING (true);
INSERT INTO sdu.t VALUES (1, 'one');
SELECT * FROM dolt_status WHERE table_name LIKE 'sdu.%' OR table_name LIKE 'sdu_own%';
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::sdu.t);
SELECT name, dirty FROM dolt_branches WHERE name = 'main';
SELECT dolt_commit('-am', 'refused');
DROP SCHEMA sdu CASCADE;
SELECT length(dolt_commit('-Am', 'sdu gone')::text) AS committed;

-- A diff of one table reads that table's changes alone, however many
-- others changed.
CREATE SCHEMA sdo VERSIONED;
CREATE TABLE sdo.one (id int PRIMARY KEY, v text);
CREATE TABLE sdo.other (id int PRIMARY KEY, v text);
INSERT INTO sdo.one VALUES (1, 'a');
INSERT INTO sdo.other SELECT g, 'x' FROM generate_series(1, 5000) g;
SELECT length(dolt_commit('-Am', 'sdo')::text) AS committed;
UPDATE sdo.one SET v = 'b';
UPDATE sdo.other SET v = 'y';
SELECT count(*) AS reset FROM test_fold_counters(true);
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::sdo.one);
SELECT pages < 10 AS only_its_pages, full_tables FROM test_fold_counters(true);
SELECT count(*) FROM dolt_diff('WORKING', 'WORKING', NULL::sdo.one);
SELECT pages < 10 AS only_its_pages FROM test_fold_counters(true);
DROP SCHEMA sdo CASCADE;
SELECT length(dolt_commit('-Am', 'sdo gone')::text) AS committed;

-- dolt_diff takes a table's row type; a large value diffs whole.
SELECT * FROM dolt_diff('HEAD', 'WORKING', NULL::int);
CREATE SCHEMA sdt VERSIONED;
CREATE TABLE sdt.t (id int PRIMARY KEY, big text);
INSERT INTO sdt.t VALUES (1, repeat('a', 6000));
SELECT length(dolt_commit('-Am', 'sdt')::text) AS committed;
UPDATE sdt.t SET big = repeat('b', 7000);
SELECT diff_type, length((from_row).big), left((from_row).big, 1),
	length((to_row).big), left((to_row).big, 1)
  FROM dolt_diff('HEAD', 'WORKING', NULL::sdt.t);

-- an aggregate, or a row security policy, makes a commit fail, each alone
CREATE AGGREGATE sdt.mysum(int) (SFUNC = int4pl, STYPE = int);
SELECT dolt_commit('-Am', 'aggregate');
DROP AGGREGATE sdt.mysum(int);
ALTER TABLE sdt.t ENABLE ROW LEVEL SECURITY;
CREATE POLICY sdt_pol ON sdt.t USING (true);
SELECT dolt_commit('-Am', 'policy');
DROP SCHEMA sdt CASCADE;
SELECT length(dolt_commit('-Am', 'sdt gone')::text) AS committed;
