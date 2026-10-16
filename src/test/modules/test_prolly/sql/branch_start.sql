-- A branch starts at its start point's head commit, as in Dolt: made from
-- a branch with uncommitted changes, rows or objects, it has none of them,
-- and they stay the other branch's.  Whether made by dolt_branch or
-- dolt_checkout -b, from the session's branch or another.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA bs VERSIONED;
CREATE TABLE bs.t (id int PRIMARY KEY, v text);
INSERT INTO bs.t VALUES (1, 'one'), (2, 'two');
SELECT length(dolt_commit('-Am', 'first')::text) AS committed;

-- uncommitted on main: a row changed, added and removed, and a new table
UPDATE bs.t SET v = 'uno' WHERE id = 1;
INSERT INTO bs.t VALUES (3, 'three');
DELETE FROM bs.t WHERE id = 2;
CREATE TABLE bs.u (a int PRIMARY KEY);
INSERT INTO bs.u VALUES (1);

SELECT dolt_branch('b');
SELECT (dolt_checkout('-b', 'c'))[2];
SELECT * FROM bs.t ORDER BY id;
SELECT to_regclass('bs.u') AS u;
SELECT count(*) AS changes FROM dolt_status;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;

SELECT dolt_checkout('b') IS NOT NULL AS on_b;
SELECT * FROM bs.t ORDER BY id;
SELECT to_regclass('bs.u') AS u;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;
-- (and from a branch other than the session's, with changes of its own)
INSERT INTO bs.t VALUES (4, 'four');
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('d', 'b');

-- main still has its changes
SELECT * FROM bs.t ORDER BY id;
SELECT * FROM bs.u;
SELECT dolt_checkout('d') IS NOT NULL AS on_d;
SELECT * FROM bs.t ORDER BY id;
SELECT dolt_checkout('b') IS NOT NULL AS on_b;
SELECT * FROM bs.t ORDER BY id;

SELECT dolt_reset('--hard');
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'b', 'c', 'd');
DROP SCHEMA bs CASCADE;
