-- dolt_branch -m: renaming a branch, with its head, working set, merge in
-- progress and upstream, as in Dolt.

CREATE SCHEMA br VERSIONED;
CREATE TABLE br.t (a int PRIMARY KEY, v int);
INSERT INTO br.t VALUES (1, 0);
SELECT length(dolt_commit('-Am', 'init')::text) AS committed;

-- A branch with only its head, never used, renamed.
SELECT dolt_branch('br_head');
SELECT dolt_hashof('br_head') AS head \gset
SELECT dolt_branch('-m', 'br_head', 'br_moved');
SELECT name FROM dolt_branches WHERE name LIKE 'br\_%' ORDER BY 1;
SELECT dolt_hashof('br_moved') = :'head' AS same_head;
SELECT dolt_hashof('br_head');

-- A branch with copies and uncommitted changes keeps them.
SELECT dolt_checkout('br_moved');
INSERT INTO br.t VALUES (2, 0);
SELECT dolt_checkout('main');
SELECT dolt_branch('-m', 'br_moved', 'br_used');
SELECT dolt_checkout('br_used');
SELECT a, v FROM br.t ORDER BY a;

-- The session's own branch: it stays on it, under the new name.
SELECT dolt_branch('--move', 'br_used', 'br_mine');
SELECT active_branch();
SELECT a, v FROM br.t ORDER BY a;
SELECT length(dolt_commit('-am', 'two')::text) AS committed;
SELECT dolt_checkout('main');
SELECT a, v FROM br.t ORDER BY a;

-- A rename to its own name changes nothing; with -f too.
SELECT dolt_branch('-m', 'br_mine', 'br_mine');
SELECT dolt_branch('-mf', 'br_mine', 'br_mine');
SELECT count(*) FROM dolt_branches WHERE name = 'br_mine';

-- Refused: a branch that doesn't exist, a name taken or invalid, main,
-- and the wrong number of names.
SELECT dolt_branch('-m', 'br_nosuch', 'br_nosuch');
SELECT dolt_branch('-m', 'br_nosuch', 'br_other');
SELECT dolt_branch('-m', 'br_mine', 'main');
SELECT dolt_branch('-m', 'br_mine', 'HEAD');
SELECT dolt_branch('-m', 'br_mine', 'detached/x');
SELECT dolt_branch('-m', 'main', 'trunk');
SELECT dolt_branch('-m', 'br_mine');
SELECT dolt_branch('-m', '-d', 'br_mine', 'x');

-- A merge in progress moves with the branch.
SELECT dolt_branch('br_side');
SELECT dolt_checkout('br_side');
UPDATE br.t SET v = 10 WHERE a = 1;
SELECT length(dolt_commit('-am', 'side')::text) AS committed;
SELECT dolt_checkout('br_mine');
UPDATE br.t SET v = 20 WHERE a = 1;
SELECT length(dolt_commit('-am', 'mine')::text) AS committed;
BEGIN;
SET LOCAL dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('br_side'))[2:4];
SELECT dolt_branch('-m', 'br_mine', 'br_merging');
SELECT table_name, num_conflicts FROM dolt_conflicts();
SELECT (dolt_merge('--abort'))[4];
COMMIT;
SELECT active_branch();
SELECT a, v FROM br.t ORDER BY a;

-- The renamed branch is found again as rolled back with its transaction.
BEGIN;
SELECT dolt_branch('-m', 'br_side', 'br_gone');
ROLLBACK;
SELECT name FROM dolt_branches WHERE name LIKE 'br\_%' ORDER BY 1;

-- Forced, it replaces a branch of the new name; not one checked out.
SELECT dolt_branch('br_old');
SELECT dolt_branch('br_target');
SELECT dolt_branch('-m', 'br_old', 'br_target');
SELECT dolt_branch('-mf', 'br_old', 'br_target');
SELECT name FROM dolt_branches WHERE name LIKE 'br\_%' ORDER BY 1;
SELECT dolt_branch('-M', 'br_target', 'br_merging');	-- (checked out)
SELECT dolt_branch('-D', 'br_target');

-- "--" ends the options: names after it may begin with "-".
SELECT dolt_branch('-m', '--', 'br_side', 'br_dash');
SELECT name FROM dolt_branches WHERE name LIKE 'br\_%' ORDER BY 1;

SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'br_merging');
SELECT dolt_branch('-D', 'br_dash');
DROP SCHEMA br CASCADE;
