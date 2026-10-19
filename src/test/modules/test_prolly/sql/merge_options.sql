-- dolt_merge's --ff-only, --squash and --no-commit, what it returns when
-- there is nothing to merge, and dolt_merge_base(), as in Dolt.

CREATE SCHEMA mo VERSIONED;
CREATE TABLE mo.t (pk int PRIMARY KEY, c1 text);
CREATE TABLE mo.u (a int);
INSERT INTO mo.t VALUES (1, 'main1');
SELECT length(dolt_commit('-Am', 'main commit')::text) AS committed;
SELECT dolt_branch('mo_feature');
SELECT dolt_checkout('mo_feature');
INSERT INTO mo.t VALUES (2, 'feature1');
SELECT length(dolt_commit('-am', 'feature commit')::text) AS committed;
SELECT dolt_checkout('main');

-- The flags that can't go together.
SELECT dolt_merge('--ff-only', '--no-ff', 'mo_feature');
SELECT dolt_merge('--ff-only', '--squash', 'mo_feature');
SELECT dolt_merge('--squash', '--no-ff', 'mo_feature');
-- (a long option with one dash too, as Dolt's parser takes it)
SELECT dolt_merge('-ff-only', '-no-ff', 'mo_feature');
SELECT dolt_merge('-squash', '-no-ff', '-message', 'x', 'mo_feature');
SELECT dolt_merge('-nope', 'mo_feature');
-- (a short option with two dashes, and every function's options so)
SELECT dolt_merge('--squash', '--no-ff', '--m', 'x', 'mo_feature');
SELECT dolt_pull('-no-ff', '-message', 'x', 'mo_nope');
SELECT dolt_push('-force', '--u', 'mo_nope');
SELECT dolt_branch('-copy', '-force', 'mo_feature', 'mo_copy');
SELECT dolt_branch('-delete', '-force', 'mo_copy');
SELECT dolt_checkout('--b', 'mo_two_dash') IS NOT NULL AS created;
SELECT dolt_checkout('main') IS NOT NULL AS back;
SELECT dolt_branch('-D', 'mo_two_dash');
BEGIN;
INSERT INTO mo.u VALUES (1);
SELECT length(dolt_commit('-allow-empty', '--m', 'one dash, two')::text) AS committed;
SELECT dolt_reset('-hard', 'HEAD~1');
-- (short flags that go together, one with two dashes)
SELECT length(dolt_commit('--A', '--allow-empty', '-m', 'two-dash flag')::text) AS committed;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_branch('mo_short');
SELECT dolt_branch('--m', 'mo_short', 'mo_short2');
SELECT dolt_branch('--D', 'mo_short2');
SELECT count(*) AS left FROM dolt_branches WHERE name LIKE 'mo_short%';
SELECT dolt_verify_constraints('--a', '--o');
SELECT count(*) AS reset FROM mo.u;
ROLLBACK;

-- The merge base of two refs is the commit a merge of them starts from.
SELECT dolt_merge_base('main', 'mo_feature') = dolt_hashof('main') AS base_is_main;
SELECT dolt_merge_base('mo_feature', 'main') = dolt_hashof('main') AS either_way;
SELECT dolt_merge_base('main', NULL) IS NULL AS strict;
SELECT dolt_merge_base('main', 'mo_nope');

-- Nothing to merge: no commit id, and Dolt's messages.
SELECT (dolt_merge('main'))[1:4] AS up_to_date;

-- A squashed fast-forward brings in the rows, leaving the head and making
-- no commit.
SELECT dolt_hashof('HEAD') AS before \gset
SELECT m[1] = dolt_hashof('mo_feature') AS theirs, m[2:4]
FROM dolt_merge('mo_feature', '--squash') m;
SELECT * FROM mo.t ORDER BY pk;
SELECT dolt_hashof('HEAD') = :'before' AS head_stays;
SELECT table_name, status FROM dolt_status();
SELECT dolt_reset('--hard');

-- --ff-only fast-forwards; a merge that isn't one is refused, changing
-- nothing.
SELECT m[1] = dolt_hashof('mo_feature') AS theirs, m[2:4]
FROM dolt_merge('--ff-only', 'mo_feature') m;
SELECT (dolt_merge('mo_feature~1'))[1:4] AS ahead;
INSERT INTO mo.t VALUES (3, 'main3');
SELECT (dolt_commit('-am', 'main commit 2'))[1] AS main2 \gset
SELECT dolt_checkout('mo_feature');
INSERT INTO mo.t VALUES (4, 'feature4');
SELECT length(dolt_commit('-am', 'feature commit 2')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('--ff-only', 'mo_feature');
SELECT * FROM mo.t ORDER BY pk;

-- --no-commit merges and leaves the merge in progress, its commit
-- dolt_commit's, with both parents.
SELECT (dolt_merge('mo_feature', '--no-commit', '-m', 'unused'))[1:4] AS no_commit;
SELECT * FROM mo.t ORDER BY pk;
SELECT dolt_hashof('HEAD') = :'main2' AS no_commit_made;
SELECT dolt_merge('mo_feature');
SELECT length(dolt_commit('-m', 'merged by hand')::text) AS committed;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT dolt_hashof('HEAD^2') = dolt_hashof('mo_feature') AS second_parent;

-- --no-commit over uncommitted changes is refused: the merge's commit
-- would take them in.
SELECT dolt_checkout('mo_feature');
INSERT INTO mo.t VALUES (5, 'feature5');
SELECT length(dolt_commit('-am', 'feature commit 3')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO mo.u VALUES (1);
SELECT dolt_merge('mo_feature', '--no-commit');
DELETE FROM mo.u;

-- A squashed three-way merge commits the changes with one parent.
SELECT m[1] <> '' AS committed, m[2:4] FROM dolt_merge('mo_feature', '--squash') m;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT dolt_hashof('HEAD^2');
SELECT * FROM mo.t ORDER BY pk;

-- Squashed and not committed: the changes alone, no merge in progress.
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT (dolt_merge('mo_feature', '--squash', '--no-commit'))[1:4] AS squashed;
SELECT dolt_merge('--abort');
SELECT * FROM mo.t ORDER BY pk;
SELECT length(dolt_commit('-am', 'squashed by hand')::text) AS committed;
SELECT dolt_hashof('HEAD^2');

-- A squash's conflicts are resolved as a pick's: its commit has one
-- parent.
SELECT dolt_checkout('mo_feature');
UPDATE mo.t SET c1 = 'theirs' WHERE pk = 1;
SELECT length(dolt_commit('-am', 'feature conflict')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE mo.t SET c1 = 'ours' WHERE pk = 1;
SELECT length(dolt_commit('-am', 'main conflict')::text) AS committed;
BEGIN;
SELECT (dolt_merge('mo_feature', '--squash'))[1:4] AS conflicted;
SELECT dolt_conflicts_resolve('--theirs', 'mo.t');
SELECT length(dolt_commit('-m', 'squash resolved')::text) AS committed;
COMMIT;
SELECT dolt_hashof('HEAD^2');
SELECT c1 FROM mo.t WHERE pk = 1;

SELECT dolt_branch('-D', 'mo_feature');
DROP SCHEMA mo CASCADE;
