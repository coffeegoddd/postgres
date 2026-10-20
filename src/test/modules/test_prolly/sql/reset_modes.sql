-- dolt_reset('--soft', ref), and dolt_reset(ref) alone: the head moves to
-- ref, the working set stays as it is (there is no staging area, so soft
-- and mixed resets are one), and what differs from the new head shows as
-- changes, which a commit takes.
CREATE DATABASE regress_reset_modes;
\c regress_reset_modes
CREATE SCHEMA rm VERSIONED;
CREATE TABLE rm.t (pk int PRIMARY KEY, v text);
INSERT INTO rm.t VALUES (1, 'a');
SELECT length(dolt_commit('-Am', 'one')::text) AS committed;
INSERT INTO rm.t VALUES (2, 'b');
UPDATE rm.t SET v = 'z' WHERE pk = 1;
SELECT length(dolt_commit('-am', 'two')::text) AS committed;
SELECT dolt_hashof('HEAD') AS two \gset

-- undo the last commit, keeping its changes
SELECT dolt_reset('--soft', 'HEAD~1');
SELECT message FROM dolt_log LIMIT 1;
SELECT table_name, status FROM dolt_status;
SELECT diff_type, (from_row).v AS was, (to_row).v AS is
FROM dolt_diff('HEAD', 'WORKING', NULL::rm.t) ORDER BY coalesce((to_row).pk, (from_row).pk);
SELECT * FROM rm.t ORDER BY pk;

-- committed again: the same rows as the commit undone
SELECT length(dolt_commit('-am', 'two again')::text) AS committed;
SELECT count(*) AS differs FROM dolt_diff(:'two', 'HEAD', NULL::rm.t);

-- the plain form is the same; to a commit on no path from the head too
SELECT dolt_reset('HEAD~1');
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_reset(:'two');
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_hashof('HEAD') = :'two' AS at_two;

-- refused during a merge in progress; and for a ref that isn't one
SELECT dolt_reset('--soft', 'no_such_ref');
SELECT dolt_reset('rm.t');
SELECT dolt_reset('--hard', '--soft');
SELECT dolt_checkout('-b', 'side');
UPDATE rm.t SET v = 'side' WHERE pk = 2;
SELECT length(dolt_commit('-am', 'side')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE rm.t SET v = 'main' WHERE pk = 2;
SELECT length(dolt_commit('-am', 'main')::text) AS committed;
SET dolt.allow_commit_conflicts = on;
SELECT dolt_merge('side') IS NOT NULL AS merged;
SELECT dolt_reset('--soft', 'HEAD~1');
SELECT dolt_reset('--hard');
SELECT count(*) AS changed FROM dolt_status;

-- on a protected branch, a soft reset that would leave it with changes
-- no commit holds is refused; one to a commit of the same tree isn't,
-- nor a hard one
SELECT dolt_branch('--protect', 'main');
SELECT dolt_hashof('HEAD') AS protected_head \gset
SELECT dolt_reset('--soft', 'HEAD~1');
SELECT dolt_hashof('HEAD') = :'protected_head' AS head_kept;
SELECT count(*) AS changed FROM dolt_status;
SELECT length(dolt_commit('--allow-empty', '-m', 'same tree')::text) AS committed;
SELECT dolt_reset('--soft', 'HEAD~1');
SELECT dolt_hashof('HEAD') = :'protected_head' AS back_to_it;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_branch('--unprotect', 'main');

\c contrib_regression
DROP DATABASE regress_reset_modes;
