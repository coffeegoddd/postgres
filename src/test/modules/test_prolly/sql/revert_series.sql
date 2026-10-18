-- dolt_revert as in Dolt: one commit per reverted commit, every ref
-- resolved first; a revert that conflicts stops the series, in progress,
-- to resolve and --continue, or --abort back to where the series began.

CREATE SCHEMA rv VERSIONED;
CREATE TABLE rv.t (pk int PRIMARY KEY, c1 text);
INSERT INTO rv.t VALUES (1, 'a');
SELECT length(dolt_commit('-Am', 'one')::text) AS committed;
INSERT INTO rv.t VALUES (2, 'b');
SELECT length(dolt_commit('-am', 'two')::text) AS committed;
INSERT INTO rv.t VALUES (3, 'c');
SELECT length(dolt_commit('-am', 'three')::text) AS committed;
SELECT dolt_hashof('HEAD') AS three \gset

-- Two commits, in the order given; HEAD~1 is "two", resolved before the
-- first revert moves HEAD.  The id is the last commit's.
SELECT r[1] = dolt_hashof('HEAD') AS last, r[2:4] FROM dolt_revert('HEAD', 'HEAD~1') r;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 5;
SELECT dolt_hashof('HEAD~2') = :'three' AS one_commit_each;
SELECT * FROM rv.t ORDER BY pk;

-- A revert that conflicts, outside a transaction block, is rolled back.
SELECT dolt_reset('--hard', :'three');
UPDATE rv.t SET c1 = 'b2' WHERE pk = 2;
SELECT length(dolt_commit('-am', 'four')::text) AS committed;
UPDATE rv.t SET c1 = 'c2' WHERE pk = 3;
SELECT length(dolt_commit('-am', 'five')::text) AS committed;
SELECT dolt_hashof('HEAD') AS five \gset
SELECT dolt_revert('HEAD~2', 'HEAD~3');
SELECT dolt_hashof('HEAD') = :'five' AS unchanged;

-- In a transaction block it stops there: no commit, the tables with
-- conflicts counted.  --continue reports them again until resolved.
BEGIN;
SELECT (dolt_revert('HEAD~2', 'HEAD~3'))[1:4] AS stopped;
SELECT table_name, num_conflicts FROM dolt_conflicts();
SELECT (dolt_revert('--continue'))[1:4] AS still_conflicted;
SAVEPOINT s;
SELECT dolt_cherry_pick('--continue');		-- not a pick
ROLLBACK TO s;
SAVEPOINT s;
SELECT dolt_merge('rv_nope');				-- a merge is in progress
ROLLBACK TO s;

-- --abort goes back to the head before the series.
SELECT (dolt_revert('--abort'))[1:4] AS aborted;
SELECT dolt_hashof('HEAD') = :'five' AS back;
SELECT * FROM rv.t ORDER BY pk;

-- Resolved, --continue commits it with its message, and goes on with the
-- rest, which here conflicts too.
SELECT (dolt_revert('HEAD~2', 'HEAD~3'))[1:4] AS stopped;
SELECT dolt_conflicts_resolve('--theirs', 'rv.t');
SELECT (dolt_revert('--continue'))[1:4] AS stopped_again;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT dolt_conflicts_resolve('--theirs', 'rv.t');
SELECT r[1] = dolt_hashof('HEAD') AS last, r[2:4] FROM dolt_revert('--continue') r;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 3;
SELECT * FROM rv.t ORDER BY pk;
COMMIT;

-- An abort after one of the series committed goes back before it.
BEGIN;
UPDATE rv.t SET c1 = 'a6' WHERE pk = 1;
SELECT length(dolt_commit('-am', 'six')::text) AS committed;
SELECT dolt_hashof('HEAD') AS six \gset
INSERT INTO rv.t VALUES (2, 'b7');
SELECT length(dolt_commit('-am', 'seven')::text) AS committed;
SELECT (dolt_revert('HEAD~1', 'HEAD~2'))[1:4] AS stopped;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT (dolt_revert('--abort'))[1:4] AS aborted;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
COMMIT;

-- --author names the reverts' author.
SELECT r[2:4] FROM dolt_revert('--author', 'Ada <ada@example.com>', 'HEAD') r;
SELECT committer, email, message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;

-- Refused: nothing in progress, --abort with --continue, a ref with them.
SELECT dolt_revert('--continue');
SELECT dolt_revert('--abort');
SELECT dolt_revert('--abort', '--continue');
BEGIN;
SELECT (dolt_revert('HEAD~3'))[2:4];
SELECT dolt_revert('--continue', 'HEAD');
ROLLBACK;

-- Neither a revert nor a pick starts on a branch with constraint
-- violations (Dolt's working set holds them, and must be clean): those
-- they leave are their own.
BEGIN;
ALTER TABLE rv.t ADD CONSTRAINT t_short CHECK (length(c1) < 1) NOT VALID;
SELECT dolt_verify_constraints();
SELECT dolt_revert('HEAD');
ROLLBACK;
BEGIN;
ALTER TABLE rv.t ADD CONSTRAINT t_short CHECK (length(c1) < 1) NOT VALID;
SELECT dolt_verify_constraints();
SELECT dolt_cherry_pick('HEAD~1');
ROLLBACK;

DROP SCHEMA rv CASCADE;
