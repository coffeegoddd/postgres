-- Three-way merges of branches made by copying: each side's changes since
-- their common ancestor, conflicts where both changed a row differently.

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
CREATE TABLE app.k (a text);
CREATE TABLE app.untouched (x int PRIMARY KEY);
INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 300) g;
INSERT INTO app.k VALUES ('x'), ('y'), ('y');
INSERT INTO app.untouched VALUES (1);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');

-- main and feature change different rows, and one row alike
UPDATE app.t SET v = 'main' WHERE id IN (1, 2);
DELETE FROM app.t WHERE id = 3;
INSERT INTO app.t VALUES (301, 'main');
UPDATE app.t SET v = 'alike' WHERE id = 100;
INSERT INTO app.k VALUES ('y');
SELECT length(dolt_commit('-Am', 'on main')::text) AS committed;
SELECT dolt_checkout('feature');
UPDATE app.t SET v = 'feature' WHERE id IN (10, 20);
DELETE FROM app.t WHERE id = 30;
INSERT INTO app.t VALUES (302, 'feature');
UPDATE app.t SET v = 'alike' WHERE id = 100;
DELETE FROM app.k WHERE ctid = (SELECT min(ctid) FROM app.k WHERE a = 'y');
INSERT INTO app.k VALUES ('z');
SELECT length(dolt_commit('-Am', 'on feature')::text) AS committed;
SELECT dolt_checkout('main');

SELECT (dolt_merge('feature'))[2:4];
SELECT string_agg(message, ', ' ORDER BY message COLLATE "C") FROM
  (SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 4) s;
SELECT dolt_hashof('HEAD^1') = (SELECT commit_hash FROM dolt_log('main') WHERE message = 'on main') AS first_parent,
       dolt_hashof('HEAD^2') = dolt_hashof('feature') AS second_parent;
SELECT id, v FROM app.t WHERE id IN (1, 2, 3, 10, 20, 30, 100, 301, 302) ORDER BY id;
SELECT count(*) FROM app.t;
SELECT a, count(*) FROM app.k GROUP BY a ORDER BY a;
SELECT * FROM dolt_status();
SELECT (dolt_merge('feature'))[2:4];		-- merged already

-- Conflicts: both change row 5, differently; feature also changes row 6.
SELECT dolt_checkout('feature');
SELECT (dolt_merge('main'))[2:4];			-- feature fast-forwards
UPDATE app.t SET v = 'feature 5' WHERE id = 5;
UPDATE app.t SET v = 'feature 6' WHERE id = 6;
SELECT length(dolt_commit('-Am', 'feature 5')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.t SET v = 'main 5' WHERE id = 5;
SELECT length(dolt_commit('-Am', 'main 5')::text) AS committed;

-- outside a transaction block, the merge fails and changes nothing
SELECT dolt_merge('feature');
SELECT v FROM app.t WHERE id IN (5, 6) ORDER BY id;
SELECT * FROM dolt_conflicts();

BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT * FROM dolt_status();
SELECT * FROM dolt_conflicts();
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::app.t);
SELECT v FROM app.t WHERE id IN (5, 6) ORDER BY id;	-- ours kept; theirs merged
SAVEPOINT s;
SELECT dolt_commit('-Am', 'too soon');
ROLLBACK TO s;
COMMIT;
-- (the transaction's commit was refused, so nothing of it stayed)
SELECT v FROM app.t WHERE id IN (5, 6) ORDER BY id;

-- Resolved with theirs, then committed: a merge commit.
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_conflicts_resolve('--theirs', 'app.t');
SELECT * FROM dolt_conflicts();
SELECT v FROM app.t WHERE id IN (5, 6) ORDER BY id;
SELECT length(dolt_commit('-Am', 'resolved')::text) AS committed;
COMMIT;
SELECT dolt_hashof('HEAD^2') = dolt_hashof('feature') AS merge_commit;
SELECT * FROM dolt_status();

-- Resolved with ours, then aborted.
SELECT dolt_checkout('feature');
UPDATE app.t SET v = 'feature again' WHERE id = 7;
SELECT length(dolt_commit('-Am', 'feature 7')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.t SET v = 'main again' WHERE id = 7;
SELECT length(dolt_commit('-Am', 'main 7')::text) AS committed;
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_conflicts_resolve('--ours', '.');
SELECT v FROM app.t WHERE id = 7;
SELECT (dolt_merge('--abort'))[2:4];
SELECT v FROM app.t WHERE id = 7;
SELECT * FROM dolt_conflicts();
COMMIT;
-- with dolt.allow_commit_conflicts they can be kept
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('feature'))[2:4];
SELECT * FROM dolt_conflicts();
SELECT dolt_merge('feature');		-- one at a time
SELECT (dolt_merge('--abort'))[2:4];
RESET dolt.allow_commit_conflicts;

-- A keyless table's copies: both sides removing the same copy is alike;
-- removing more copies than there are, a conflict.
SELECT dolt_branch('-D', 'feature');
SELECT dolt_branch('feature');
DELETE FROM app.k WHERE a IN ('y', 'z');
SELECT length(dolt_commit('-Am', 'main drops y and z')::text) AS committed;
SELECT dolt_checkout('feature');
DELETE FROM app.k WHERE ctid = (SELECT min(ctid) FROM app.k WHERE a = 'y');
DELETE FROM app.k WHERE a = 'z';
INSERT INTO app.k VALUES ('w');
SELECT length(dolt_commit('-Am', 'feature drops a y and z')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::app.k);
SELECT dolt_conflicts_resolve('--theirs', 'app.k');
SELECT a, count(*) FROM app.k GROUP BY a ORDER BY a;
ROLLBACK;

-- Not yet: different definitions.
SELECT dolt_checkout('feature');
ALTER TABLE app.t ADD COLUMN w int;
SELECT length(dolt_commit('-Am', 'a column')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.t SET v = 'main 8' WHERE id = 8;
SELECT length(dolt_commit('-Am', 'main 8')::text) AS committed;
SELECT dolt_merge('feature');

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA app CASCADE;

-- A merge reads rows as they were folded: a NULL in a column added with a
-- default is NULL in the conflict's rows, not the default.
CREATE SCHEMA mgd VERSIONED;
CREATE TABLE mgd.t (id int PRIMARY KEY, a text);
INSERT INTO mgd.t VALUES (1, 'one');
ALTER TABLE mgd.t ADD COLUMN c int DEFAULT 5;
SELECT length(dolt_commit('-Am', 'c is 5')::text) AS committed;
SELECT dolt_branch('mgd_side');
SELECT dolt_checkout('mgd_side') IS NOT NULL AS on_side;
UPDATE mgd.t SET c = NULL;
SELECT length(dolt_commit('-am', 'c is NULL')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE mgd.t SET a = 'uno';
SELECT length(dolt_commit('-am', 'a is uno')::text) AS committed;
BEGIN;
SELECT (dolt_merge('mgd_side'))[2:4];
SELECT our_row, their_row FROM dolt_conflicts(NULL::mgd.t);
ROLLBACK;
SELECT dolt_branch('-D', 'mgd_side');
DROP SCHEMA mgd CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;

-- A merge doesn't read the rows of a table whose collation differs now
-- from what a version recorded.
CREATE SCHEMA mgc VERSIONED;
CREATE COLLATION mgc.c_utf8 (provider = builtin, locale = 'C.UTF-8');
CREATE TABLE mgc.t (k text COLLATE mgc.c_utf8 PRIMARY KEY, v int);
INSERT INTO mgc.t VALUES ('a', 1), ('b', 1);
SELECT length(dolt_commit('-Am', 'collated')::text) AS committed;
SELECT dolt_branch('mgc_side');
SELECT dolt_checkout('mgc_side') IS NOT NULL AS on_side;
UPDATE mgc.t SET v = 2 WHERE k = 'a';
SELECT length(dolt_commit('-am', 'a is 2')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE mgc.t SET v = 3 WHERE k = 'b';
SELECT length(dolt_commit('-am', 'b is 3')::text) AS committed;
UPDATE pg_collation SET collicurules = '&a<b' WHERE collname = 'c_utf8';
SELECT (dolt_merge('mgc_side'))[2:4];
UPDATE pg_collation SET collicurules = NULL WHERE collname = 'c_utf8';
SELECT (dolt_merge('mgc_side'))[2:4];
SELECT * FROM mgc.t ORDER BY k;
SELECT dolt_branch('-D', 'mgc_side');
DROP SCHEMA mgc CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;
