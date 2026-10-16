-- Moving a branch made by copying to a commit: reset, a branch at a
-- commit, and fast-forward merges write the rows that differ.

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text, u int UNIQUE);
CREATE TABLE app.k (a text);
CREATE TABLE app.log (what text);
CREATE FUNCTION app.logged() RETURNS trigger LANGUAGE plpgsql AS
$$BEGIN INSERT INTO app.log VALUES (TG_OP); RETURN COALESCE(NEW, OLD); END$$;
CREATE TRIGGER t_trig BEFORE INSERT OR UPDATE OR DELETE ON app.t
  FOR EACH ROW EXECUTE FUNCTION app.logged();
INSERT INTO app.t SELECT g, 'v' || g, g FROM generate_series(1, 500) g;
INSERT INTO app.k VALUES ('x'), ('y'), ('y');
DELETE FROM app.log;
SELECT (dolt_commit('-Am', 'first'))[1] AS c1 \gset

UPDATE app.t SET v = 'changed' WHERE id <= 3;
DELETE FROM app.t WHERE id BETWEEN 10 AND 12;
INSERT INTO app.t VALUES (501, 'new', 501);
-- a row taking a unique value another row gives up
UPDATE app.t SET u = -20 WHERE id = 20;
UPDATE app.t SET u = 20 WHERE id = 21;
INSERT INTO app.k VALUES ('y'), ('z');
DELETE FROM app.k WHERE a = 'x';
DELETE FROM app.log;
SELECT (dolt_commit('-Am', 'second'))[1] AS c2 \gset
SELECT count(*), sum(u) FROM app.t;

-- reset --hard to the first commit: its rows, no triggers fired
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_hashof('HEAD') = :'c1' AS head_is_first;
SELECT count(*), sum(u) FROM app.t;
SELECT * FROM app.t WHERE id IN (1, 2, 3, 10, 11, 12, 20, 21, 501) ORDER BY id;
SELECT a, count(*) FROM app.k GROUP BY a ORDER BY a;
SELECT count(*) AS triggered FROM app.log;
SELECT * FROM dolt_status();
SELECT prolly_root_hash('HEAD') = prolly_root_hash(:'c1') AS same_root;

-- reset --hard HEAD drops uncommitted changes
UPDATE app.t SET v = 'uncommitted' WHERE id = 5;
DELETE FROM app.k;
SELECT dolt_reset('--hard');
SELECT v FROM app.t WHERE id = 5;
SELECT count(*) FROM app.k;
SELECT * FROM dolt_status();

-- and goes with its transaction
BEGIN;
SELECT dolt_reset('--hard', :'c2');
SELECT count(*) FROM app.t;
ROLLBACK;
SELECT dolt_hashof('HEAD') = :'c1' AS still_first, count(*) FROM app.t;

-- A branch at a commit.
SELECT dolt_branch('at_second', :'c2');
SELECT dolt_hashof('at_second') = :'c2' AS at_second;
SELECT dolt_checkout('at_second');
SELECT count(*), sum(u) FROM app.t;
SELECT * FROM dolt_status();
SELECT dolt_checkout('-b', 'at_first', 'main');
SELECT count(*) FROM app.t;

-- Fast-forward: main is behind at_second.
SELECT dolt_checkout('main');
SELECT (dolt_merge('at_first'))[2:4];		-- merged already
SELECT (dolt_merge('at_second'))[2:4];
SELECT dolt_hashof('HEAD') = :'c2' AS fast_forwarded, count(*), sum(u) FROM app.t;
SELECT * FROM dolt_status();
-- not over uncommitted changes
SELECT dolt_reset('--hard', :'c1');
UPDATE app.t SET v = 'mine' WHERE id = 1;
SELECT (dolt_merge('at_second'))[2:4];
SELECT dolt_reset('--hard');

-- Other definitions: the branch moves to them too; diverged branches whose
-- definitions differ merge their rows as the merged definition has them
-- (here, row 1 changed on both: a conflict).
SELECT dolt_checkout('at_first');
ALTER TABLE app.t ADD COLUMN w int;
SELECT length(dolt_commit('-Am', 'a column')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('at_first'))[2:4];
SELECT dolt_reset('--hard', 'at_first');
UPDATE app.t SET v = 'diverged' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'diverged')::text) AS committed;
SELECT (dolt_merge('at_second'))[2:4];
SELECT (dolt_merge('--no-ff', 'at_second'))[2:4];

SELECT dolt_branch('-D', 'at_first', 'at_second');
DROP SCHEMA app CASCADE;

-- A move writes a NULL in a column added with a default as NULL, not the
-- default (the commit's rows are as a rewrite has them).
CREATE SCHEMA mhd VERSIONED;
CREATE TABLE mhd.t (id int PRIMARY KEY, a text);
INSERT INTO mhd.t VALUES (1, 'one');
ALTER TABLE mhd.t ADD COLUMN c int DEFAULT 5;
UPDATE mhd.t SET c = NULL;
SELECT length(dolt_commit('-Am', 'a NULL in c')::text) AS committed;
UPDATE mhd.t SET c = 7;
SELECT dolt_reset('--hard');
SELECT id, c IS NULL AS c_is_null FROM mhd.t;
DROP SCHEMA mhd CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;

-- A move doesn't write the rows of a table whose collation differs now
-- from what the commit recorded (its keys may be in another order).
CREATE SCHEMA mhc VERSIONED;
CREATE COLLATION mhc.c_utf8 (provider = builtin, locale = 'C.UTF-8');
CREATE TABLE mhc.t (k text COLLATE mhc.c_utf8 PRIMARY KEY, v int);
INSERT INTO mhc.t VALUES ('a', 1);
SELECT length(dolt_commit('-Am', 'collated')::text) AS committed;
UPDATE mhc.t SET v = 2;
UPDATE pg_collation SET collicurules = '&a<b' WHERE collname = 'c_utf8';
SELECT dolt_reset('--hard');
UPDATE pg_collation SET collicurules = NULL WHERE collname = 'c_utf8';
SELECT dolt_reset('--hard');
SELECT * FROM mhc.t;
DROP SCHEMA mhc CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;

-- Merges into a branch with no commit yet, and between branches with no
-- history in common.
CREATE DATABASE mh_fresh TEMPLATE template0;
\c mh_fresh
CREATE SCHEMA app VERSIONED;
SELECT dolt_branch('other');
SELECT dolt_checkout('other') IS NOT NULL AS on_other;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
INSERT INTO app.t VALUES (1, 'other');
SELECT length(dolt_commit('-Am', 'on other')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
-- main has no commit: what it has is uncommitted, and isn't overwritten
CREATE TABLE app.t (id int PRIMARY KEY, v text);
INSERT INTO app.t VALUES (1, 'mine'), (2, 'mine');
SELECT dolt_merge('other');
SELECT * FROM app.t ORDER BY id;
-- nor merged with --no-ff, there being nothing to make a merge commit on
SELECT dolt_merge('--no-ff', 'other');
-- committed, main's history and other's have nothing in common
SELECT length(dolt_commit('-Am', 'on main')::text) AS committed;
SELECT dolt_merge('other');
\c contrib_regression
DROP DATABASE mh_fresh;

-- --no-ff where a fast-forward would do: a merge commit
CREATE SCHEMA mhf VERSIONED;
CREATE TABLE mhf.t (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'mhf')::text) AS committed;
SELECT dolt_branch('mh_ahead');
SELECT dolt_checkout('mh_ahead') IS NOT NULL AS on_ahead;
INSERT INTO mhf.t VALUES (1);
SELECT length(dolt_commit('-am', 'ahead')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('--no-ff', 'mh_ahead'))[2:4];
SELECT count(*) FROM mhf.t;
SELECT dolt_branch('-D', 'mh_ahead');
DROP SCHEMA mhf CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;

-- A materialized view's rows move too, as the commit holds them, its
-- indexes kept up; the working set then folds to the commit's root.
CREATE SCHEMA mhm VERSIONED;
CREATE TABLE mhm.t (id int PRIMARY KEY, v int);
INSERT INTO mhm.t VALUES (1, 1), (2, 2), (3, 2);
CREATE MATERIALIZED VIEW mhm.sums AS SELECT sum(v) AS s FROM mhm.t;
CREATE MATERIALIZED VIEW mhm.vs AS SELECT v FROM mhm.t;
CREATE MATERIALIZED VIEW mhm.ids AS SELECT id, v FROM mhm.t;
CREATE UNIQUE INDEX ON mhm.ids (id);
SELECT length(dolt_commit('-Am', 'views')::text) AS committed;
UPDATE mhm.t SET v = 10 WHERE id = 1;
REFRESH MATERIALIZED VIEW mhm.sums;
REFRESH MATERIALIZED VIEW mhm.vs;
REFRESH MATERIALIZED VIEW mhm.ids;
SELECT dolt_reset('--hard');
SELECT * FROM mhm.sums;
SELECT v, count(*) FROM mhm.vs GROUP BY v ORDER BY v;
SET enable_seqscan = off;
SELECT * FROM mhm.ids WHERE id = 1;
RESET enable_seqscan;
SELECT dolt_commit('-Am', 'nothing') IS NULL AS committed;
-- refreshing takes MAINTAIN, and so does moving its rows
CREATE ROLE mhm_writer;
GRANT USAGE ON SCHEMA mhm TO mhm_writer;
GRANT ALL ON mhm.t TO mhm_writer;
GRANT SELECT ON mhm.sums, mhm.vs, mhm.ids TO mhm_writer;
UPDATE mhm.t SET v = 10 WHERE id = 1;
REFRESH MATERIALIZED VIEW mhm.sums;
SET ROLE mhm_writer;
SELECT dolt_reset('--hard');
RESET ROLE;
SELECT dolt_reset('--hard');
SELECT * FROM mhm.sums;
DROP SCHEMA mhm CASCADE;
DROP ROLE mhm_writer;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;
