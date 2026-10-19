-- A keyless UPDATE is a removal and an unrelated insertion.  If a merge
-- refuses an insertion, retain the table's rows rather than losing the
-- old version; there is no row identity with which to pair the changes.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA kv VERSIONED;
CREATE TABLE kv.u (x int UNIQUE, y int);
CREATE TABLE kv.s (x int, y int);
CREATE TABLE kv.c (x int, y int);
CREATE TABLE kv.d (x int, y int);
CREATE TABLE kv.ok (x int UNIQUE, y int);
CREATE TABLE kv.sibling (pk int PRIMARY KEY);
INSERT INTO kv.u VALUES (1, 1), (2, 2);
INSERT INTO kv.s VALUES (1, 1), (2, 2);
INSERT INTO kv.c VALUES (2, 2), (2, 2);
INSERT INTO kv.d VALUES (2, 2), (2, 2);
INSERT INTO kv.ok VALUES (1, 1), (2, 2);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('other');
-- u and d use merge_table; s and c use the streaming apply_rows path.
INSERT INTO kv.u VALUES (5, 9);
CREATE UNIQUE INDEX s_x ON kv.s (x);
ALTER TABLE kv.c ADD CONSTRAINT small CHECK (x < 5);
ALTER TABLE kv.d ADD CONSTRAINT small CHECK (x < 5);
INSERT INTO kv.d VALUES (3, 3);
INSERT INTO kv.ok VALUES (9, 9);
SELECT length(dolt_commit('-am', 'ours')::text) AS committed;
SELECT dolt_checkout('other');
UPDATE kv.u SET x = 5 WHERE x = 2;
UPDATE kv.s SET x = 1 WHERE x = 2;
UPDATE kv.c SET x = 5;
UPDATE kv.d SET x = 5;
-- Even acceptable inserts in a refused table are rolled back together.
INSERT INTO kv.u VALUES (7, 7);
INSERT INTO kv.s VALUES (7, 7);
INSERT INTO kv.c VALUES (4, 4);
INSERT INTO kv.d VALUES (4, 4);
UPDATE kv.ok SET y = y + 1;
INSERT INTO kv.sibling VALUES (1);
SELECT length(dolt_commit('-am', 'theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SAVEPOINT before_merge;
SELECT (dolt_merge('other'))[3:4];
ROLLBACK TO before_merge;
SELECT count(*) AS no_violations FROM dolt_constraint_violations();
SELECT (dolt_merge('other'))[3:4];
SELECT * FROM kv.u ORDER BY x, y;
SELECT * FROM kv.s ORDER BY x, y;
SELECT * FROM kv.c ORDER BY x, y;
SELECT * FROM kv.d ORDER BY x, y;
SELECT * FROM kv.ok ORDER BY x, y;
SELECT * FROM kv.sibling;
SELECT table_name, num_violations FROM dolt_constraint_violations()
ORDER BY table_name;
SELECT violation_type, violating_row FROM dolt_constraint_violations('kv.u');
SELECT violation_type, violating_row FROM dolt_constraint_violations('kv.s');
SELECT violation_type, violating_row FROM dolt_constraint_violations('kv.c');
SELECT violation_type, violating_row FROM dolt_constraint_violations('kv.d');
SET LOCAL dolt.allow_commit_conflicts = on;
SELECT length(dolt_commit('-am', 'retain ours with violations')::text) AS committed;
COMMIT;
-- The preserved rows survive folding and moving away and back.
SELECT dolt_branch('saved_merge');
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_reset('--hard', 'saved_merge');
SELECT test_fold_root() = prolly_root_hash('saved_merge') AS same_root;
SELECT * FROM kv.u ORDER BY x, y;
SELECT * FROM kv.c ORDER BY x, y;
SELECT dolt_branch('-D', 'other');
SELECT dolt_branch('-D', 'saved_merge');
DROP SCHEMA kv CASCADE;
