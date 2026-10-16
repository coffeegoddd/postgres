-- Merges of branches made by copying whose definitions changed: objects
-- merged one by one, as the side that changed each has it; then rows.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
CREATE TABLE app.u (id int PRIMARY KEY, w text);
CREATE TABLE app.d (x int);
INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 5) g;
INSERT INTO app.u SELECT g, 'w' || g FROM generate_series(1, 5) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');

-- feature: a new table, routine and view, a column, a dropped table
SELECT dolt_checkout('feature');
CREATE TABLE app.n (id int PRIMARY KEY, t int REFERENCES app.t);
INSERT INTO app.n VALUES (1, 1), (2, 2);
CREATE FUNCTION app.twice(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 2';
ALTER TABLE app.t ADD COLUMN extra int DEFAULT 0;
UPDATE app.t SET extra = id;
CREATE VIEW app.tv AS SELECT id, app.twice(extra) AS e FROM app.t;
DROP TABLE app.d;
CREATE TABLE app.same (k int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'feature defines')::text) AS committed;

-- main: rows of another table, and the same table made alike
SELECT dolt_checkout('main');
UPDATE app.u SET w = 'main' WHERE id = 1;
INSERT INTO app.u VALUES (6, 'six');
CREATE TABLE app.same (k int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'main rows')::text) AS committed;

SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_hashof('HEAD^2') = dolt_hashof('feature') AS merge_commit;
SELECT * FROM dolt_status();
SELECT * FROM app.t ORDER BY id;
SELECT * FROM app.tv ORDER BY id;
SELECT * FROM app.n ORDER BY id;
SELECT * FROM app.u ORDER BY id;
SELECT to_regclass('app.d') IS NULL AS d_dropped, to_regclass('app.same') IS NOT NULL AS same_made;
INSERT INTO app.n VALUES (3, 99);		-- its foreign key came too
-- the merge's root is the branch folded
SELECT test_fold_root() = prolly_root_hash('HEAD') AS same_root;

-- An object both sides changed differently: a schema conflict.
SELECT dolt_checkout('feature');
SELECT (dolt_merge('main'))[2:4];		-- feature catches up first
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 1';
SELECT length(dolt_commit('-Am', 'feature f')::text) AS committed;
SELECT dolt_checkout('main');
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 2';
SELECT length(dolt_commit('-Am', 'main f')::text) AS committed;
SELECT dolt_merge('feature');
SELECT to_regprocedure('app.f()') IS NOT NULL AS ours_kept, app.f();

-- A table redefined on one side, its rows changed on the other: a column
-- added empty leaves the rows as they were, so they merge.
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'HEAD~1');
ALTER TABLE app.u ADD COLUMN more int;
SELECT length(dolt_commit('-Am', 'feature redefines u')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.u SET w = 'main again' WHERE id = 2;
SELECT length(dolt_commit('-Am', 'main changes u')::text) AS committed;
SELECT (dolt_merge('feature'))[2:4];
SELECT * FROM app.u WHERE id <= 2 ORDER BY id;
SELECT * FROM dolt_status();
-- One with a default changes every row: not yet.
SELECT dolt_checkout('feature');
SELECT (dolt_merge('main'))[2:4];
ALTER TABLE app.u ADD COLUMN most int DEFAULT 5;
SELECT length(dolt_commit('-Am', 'feature fills u')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.u SET w = 'main once more' WHERE id = 3;
SELECT length(dolt_commit('-Am', 'main changes u again')::text) AS committed;
SELECT dolt_merge('feature');
SELECT * FROM dolt_status();

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA app CASCADE;
