-- Cherry-picks and reverts on branches made by copying: a commit's
-- changes from its parent, merged three-way into the branch and committed.

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
CREATE TABLE app.k (a text);
INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 10) g;
INSERT INTO app.k VALUES ('x');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');
SELECT dolt_checkout('feature');
UPDATE app.t SET v = 'one' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'feature one')::text) AS committed;
INSERT INTO app.t VALUES (11, 'eleven');
DELETE FROM app.t WHERE id = 2;
INSERT INTO app.k VALUES ('x');
SELECT (dolt_commit('-Am', 'feature two'))[1] AS two \gset
SELECT dolt_checkout('main');
UPDATE app.t SET v = 'main' WHERE id = 5;
SELECT length(dolt_commit('-Am', 'on main')::text) AS committed;

-- Only the picked commit's changes, not its parent's.
SELECT (dolt_cherry_pick(:'two'))[2:4];
SELECT id, v FROM app.t WHERE id IN (1, 2, 5, 11) ORDER BY id;
SELECT a, count(*) FROM app.k GROUP BY a;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT dolt_hashof('HEAD^2');		-- one parent
SELECT * FROM dolt_status();

-- Reverting it undoes them.
SELECT dolt_revert('HEAD');
SELECT id, v FROM app.t WHERE id IN (1, 2, 5, 11) ORDER BY id;
SELECT a, count(*) FROM app.k GROUP BY a;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;

-- A conflict is refused, and so are uncommitted changes and merges.
UPDATE app.t SET v = 'main one' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'main one')::text) AS committed;
SELECT dolt_cherry_pick('feature~1');
UPDATE app.t SET v = 'uncommitted' WHERE id = 3;
SELECT dolt_cherry_pick(:'two');
SELECT dolt_reset('--hard');
SELECT dolt_merge('feature', '-m', 'merged');
SELECT dolt_conflicts_resolve('--theirs', '.');
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_conflicts_resolve('--theirs', '.');
SELECT length(dolt_commit('-Am', 'merged')::text) AS committed;
COMMIT;
SELECT dolt_cherry_pick('HEAD');		-- a merge commit
SELECT dolt_revert('nope');

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA app CASCADE;
