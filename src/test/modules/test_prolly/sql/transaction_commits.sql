-- dolt.transaction_commit: a Dolt commit of every SQL transaction that
-- changes a branch, made as it commits, and undone with it.
CREATE SCHEMA tc VERSIONED;
CREATE TABLE tc.t (id int PRIMARY KEY, v text);
CREATE TABLE tc_plain (id int);
SELECT length(dolt_commit('-Am', 'tc base')::text) AS committed;
CREATE FUNCTION pg_temp.commits() RETURNS bigint LANGUAGE sql
  AS $$ SELECT count(*) FROM dolt_log('main') $$;
SELECT pg_temp.commits() AS before \gset
SET dolt.transaction_commit = on;

-- A statement of its own (autocommit) commits; so does a block, once.
INSERT INTO tc.t VALUES (1, 'a');
SELECT pg_temp.commits() - :before AS commits,
	(SELECT message FROM dolt_log('main') LIMIT 1) AS message;
BEGIN;
INSERT INTO tc.t VALUES (2, 'b');
UPDATE tc.t SET v = 'B' WHERE id = 2;
COMMIT;
SELECT pg_temp.commits() - :before AS commits;
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';

-- What doesn't commit makes none: a rollback, a savepoint rolled back, an
-- error caught, an error at commit (a deferred constraint).
BEGIN;
INSERT INTO tc.t VALUES (3, 'c');
ROLLBACK;
BEGIN;
SAVEPOINT s;
INSERT INTO tc.t VALUES (3, 'c');
ROLLBACK TO s;
COMMIT;
DO $$
BEGIN
	INSERT INTO tc.t VALUES (3, 'c');
	RAISE EXCEPTION 'undo';
EXCEPTION WHEN raise_exception THEN
	NULL;
END $$;
CREATE TABLE tc.child (id int PRIMARY KEY,
	parent int REFERENCES tc.t DEFERRABLE INITIALLY DEFERRED);
SELECT pg_temp.commits() AS before \gset
BEGIN;
INSERT INTO tc.child VALUES (1, 99);
COMMIT;
SELECT pg_temp.commits() - :before AS commits;

-- Nor does a transaction that changes nothing of a branch: reads,
-- unversioned writes, and a row inserted and deleted again.
SELECT count(*) FROM tc.t;
INSERT INTO tc_plain VALUES (1);
BEGIN;
INSERT INTO tc.t VALUES (4, 'gone');
DELETE FROM tc.t WHERE id = 4;
COMMIT;
SELECT pg_temp.commits() - :before AS commits;

-- Definitions alone commit; the message is the setting's at COMMIT.
BEGIN;
CREATE TABLE tc.u (id int);
SET LOCAL dolt.transaction_commit_message = 'made u';
COMMIT;
SELECT pg_temp.commits() - :before AS commits,
	(SELECT message FROM dolt_log('main') LIMIT 1) AS message;

-- A dolt_commit() in the transaction commits what came before it; the
-- transaction's commit holds only what came after, if anything did.
SELECT pg_temp.commits() AS before \gset
BEGIN;
INSERT INTO tc.t VALUES (5, 'e');
SELECT length(dolt_commit('-m', 'mine')::text) AS committed;
COMMIT;
BEGIN;
INSERT INTO tc.t VALUES (6, 'f');
SELECT length(dolt_commit('-m', 'mine again')::text) AS committed;
INSERT INTO tc.t VALUES (7, 'g');
COMMIT;
SELECT pg_temp.commits() - :before AS commits;
SELECT message FROM dolt_log('main') LIMIT 3;

-- Two branches changed, or one left, can't commit so.
SELECT dolt_branch('tc_f');
BEGIN;
INSERT INTO tc.t VALUES (8, 'main');
SELECT dolt_checkout('tc_f');
INSERT INTO tc.t VALUES (8, 'f');
COMMIT;
SELECT active_branch();
BEGIN;
SELECT dolt_checkout('tc_f');
INSERT INTO tc.t VALUES (8, 'f');
SELECT dolt_checkout('main');
COMMIT;
SELECT dolt_checkout('main');

-- A branch committed by a version-control call is that call's: integrating
-- one's own branch makes its commits, and no other.
SELECT pg_temp.commits() AS before \gset
BEGIN;
SELECT dolt_checkout('tc_f');
INSERT INTO tc.t VALUES (9, 'f');
SELECT (dolt_integrate('main', '-m', 'f in'))[2] AS fast_forward;
COMMIT;
SELECT dolt_checkout('main');
SELECT pg_temp.commits() - :before AS commits;

-- Conflicts kept (dolt.allow_commit_conflicts): the transaction commits,
-- without a Dolt commit.
SELECT dolt_checkout('tc_f');
UPDATE tc.t SET v = 'on f' WHERE id = 1;
SELECT dolt_checkout('main');
UPDATE tc.t SET v = 'on main' WHERE id = 1;
SELECT pg_temp.commits() AS before \gset
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('tc_f'))[3] AS conflicts;
RESET dolt.allow_commit_conflicts;
SELECT pg_temp.commits() - :before AS commits;
SELECT dolt_merge('--abort') IS NOT NULL AS aborted;

-- Off again, writes wait for dolt_commit().
RESET dolt.transaction_commit;
INSERT INTO tc.t VALUES (10, 'later');
SELECT pg_temp.commits() - :before AS commits;
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'tc_f');
DROP TABLE tc_plain;
DROP SCHEMA tc CASCADE;
SELECT length(dolt_commit('-Am', 'tc gone')::text) AS committed;
