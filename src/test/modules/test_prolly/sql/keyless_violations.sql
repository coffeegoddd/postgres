-- A keyless UPDATE is a removal and an unrelated insertion.  If a merge
-- removes rows of a keyless table and refuses a row it writes, it can't
-- keep the branch's rows and theirs as merged: there is no row identity
-- with which to pair them.  The merge is refused, the branch unchanged,
-- rather than losing one side's rows.
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
SELECT dolt_branch('theirs_u');
SELECT dolt_branch('theirs_s');
SELECT dolt_branch('theirs_c');
SELECT dolt_branch('theirs_d');
SELECT dolt_branch('theirs_ok');
-- u and d use merge_table; s and c use the streaming apply_rows path.
INSERT INTO kv.u VALUES (5, 9);
CREATE UNIQUE INDEX s_x ON kv.s (x);
ALTER TABLE kv.c ADD CONSTRAINT small CHECK (x < 5);
ALTER TABLE kv.d ADD CONSTRAINT small CHECK (x < 5);
INSERT INTO kv.d VALUES (3, 3);
INSERT INTO kv.ok VALUES (9, 9);
SELECT (dolt_commit('-am', 'ours'))[1] AS ours \gset

-- Each side's change to one table, with an acceptable insert beside the
-- refused row and changes to other tables: none of it may be lost.
SELECT dolt_checkout('theirs_u');
UPDATE kv.u SET x = 5 WHERE x = 2;
INSERT INTO kv.u VALUES (7, 7);
INSERT INTO kv.sibling VALUES (1);
SELECT length(dolt_commit('-am', 'theirs u')::text) AS committed;
SELECT dolt_checkout('theirs_s');
UPDATE kv.s SET x = 1 WHERE x = 2;
INSERT INTO kv.s VALUES (7, 7);
INSERT INTO kv.sibling VALUES (1);
SELECT length(dolt_commit('-am', 'theirs s')::text) AS committed;
SELECT dolt_checkout('theirs_c');
UPDATE kv.c SET x = 5;
INSERT INTO kv.c VALUES (4, 4);
INSERT INTO kv.sibling VALUES (1);
SELECT length(dolt_commit('-am', 'theirs c')::text) AS committed;
SELECT dolt_checkout('theirs_d');
UPDATE kv.d SET x = 5;
INSERT INTO kv.d VALUES (4, 4);
INSERT INTO kv.sibling VALUES (1);
SELECT length(dolt_commit('-am', 'theirs d')::text) AS committed;
SELECT dolt_checkout('main');

SELECT dolt_merge('theirs_u');
SELECT dolt_merge('theirs_s');
SELECT dolt_merge('theirs_c');
SELECT dolt_merge('theirs_d');
-- (inside a transaction too, and retried after a savepoint)
BEGIN;
SAVEPOINT before_merge;
SELECT dolt_merge('theirs_u');
ROLLBACK TO before_merge;
SELECT dolt_merge('theirs_c');
ROLLBACK;

-- Each refused merge left the branch as it was: rows, violations, root.
SELECT test_fold_root() = prolly_root_hash(:'ours') AS same_root;
SELECT * FROM dolt_status();
SELECT count(*) AS no_violations FROM dolt_constraint_violations();
SELECT * FROM kv.u ORDER BY x, y;
SELECT * FROM kv.s ORDER BY x, y;
SELECT * FROM kv.c ORDER BY x, y;
SELECT * FROM kv.d ORDER BY x, y;
SELECT * FROM kv.sibling;

-- With no removals, a refused row is recorded alone and the rest merged,
-- as for a keyed table; a keyless table whose removals break nothing
-- merges as usual.
SELECT dolt_checkout('theirs_ok');
UPDATE kv.ok SET y = y + 1;
INSERT INTO kv.u VALUES (5, 5), (6, 6);
INSERT INTO kv.sibling VALUES (1);
SELECT length(dolt_commit('-am', 'theirs ok')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('theirs_ok'))[3:4];
SELECT * FROM kv.u ORDER BY x, y;
SELECT * FROM kv.ok ORDER BY x, y;
SELECT * FROM kv.sibling;
SELECT violation_type, violating_row FROM dolt_constraint_violations('kv.u');
SET LOCAL dolt.allow_commit_conflicts = on;
SELECT length(dolt_commit('-am', 'merged with a violation')::text) AS committed;
COMMIT;
-- The merged rows survive folding and moving away and back.
SELECT dolt_branch('saved_merge');
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_reset('--hard', 'saved_merge');
SELECT test_fold_root() = prolly_root_hash('saved_merge') AS same_root;
SELECT * FROM kv.u ORDER BY x, y;
SELECT * FROM kv.ok ORDER BY x, y;
SELECT dolt_branch('-D', 'theirs_u');
SELECT dolt_branch('-D', 'theirs_s');
SELECT dolt_branch('-D', 'theirs_c');
SELECT dolt_branch('-D', 'theirs_d');
SELECT dolt_branch('-D', 'theirs_ok');
SELECT dolt_branch('-D', 'saved_merge');
DROP SCHEMA kv CASCADE;
