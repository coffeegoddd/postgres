-- Merges into a working set with uncommitted changes, as in Dolt: allowed
-- when the merge changes none of the tables and sequences they changed,
-- and no definitions change; they stay uncommitted, left out of the
-- merge's commit.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA dm VERSIONED;
CREATE TABLE dm.a (id int PRIMARY KEY, v text);
CREATE TABLE dm.b (id int PRIMARY KEY, v text);
CREATE SEQUENCE dm.s1;
CREATE SEQUENCE dm.s2;
INSERT INTO dm.a SELECT g, 'a' || g FROM generate_series(1, 5) g;
INSERT INTO dm.b SELECT g, 'b' || g FROM generate_series(1, 5) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');
SELECT dolt_checkout('feature');
UPDATE dm.a SET v = 'feature' WHERE id = 1;
SELECT nextval('dm.s1');
SELECT length(dolt_commit('-am', 'feature changes a')::text) AS committed;
SELECT dolt_checkout('main');

-- A fast-forward over changes to another table keeps them.
UPDATE dm.b SET v = 'mine' WHERE id = 2;
SELECT nextval('dm.s2');
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_hashof('HEAD') = dolt_hashof('feature') AS fast_forwarded;
SELECT id, v FROM dm.a WHERE id = 1;
SELECT id, v FROM dm.b WHERE id = 2;
SELECT last_value FROM dm.s1;
SELECT table_name, status FROM dolt_status ORDER BY 1;

-- Over changes to the same table, it is refused.
SELECT dolt_reset('--hard', 'HEAD~1');
UPDATE dm.a SET v = 'mine' WHERE id = 3;
SELECT (dolt_merge('feature'))[2:4];
SELECT id, v FROM dm.a WHERE id IN (1, 3) ORDER BY id;
-- and over a sequence's value the merge changes
SELECT dolt_reset('--hard');
SELECT nextval('dm.s1');
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_reset('--hard');

-- A three-way merge commits the merge alone.
UPDATE dm.a SET v = 'main' WHERE id = 5;
SELECT length(dolt_commit('-am', 'main changes a')::text) AS committed;
UPDATE dm.b SET v = 'still mine' WHERE id = 4;
SELECT (dolt_merge('feature', '-m', 'merged'))[3:4];
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT diff_type, (to_row).id FROM dolt_diff('HEAD~1', 'HEAD', NULL::dm.a) ORDER BY 2;
SELECT count(*) AS b_unchanged FROM dolt_diff('HEAD~1', 'HEAD', NULL::dm.b);
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT id, v FROM dm.b WHERE id = 4;
SELECT length(dolt_commit('-am', 'mine')::text) AS committed;

-- A cherry-pick too.
SELECT dolt_checkout('feature');
UPDATE dm.a SET v = 'picked' WHERE id = 2;
SELECT length(dolt_commit('-am', 'to pick')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE dm.b SET v = 'uncommitted' WHERE id = 1;
SELECT length(dolt_cherry_pick('feature')::text) > 0 AS picked;
SELECT count(*) AS b_left_out FROM dolt_diff('HEAD~1', 'HEAD', NULL::dm.b);
SELECT table_name FROM dolt_status;

-- Uncommitted changes of definitions aren't merged into.
SELECT dolt_reset('--hard');
ALTER TABLE dm.b ADD COLUMN w int;
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_reset('--hard');

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA dm CASCADE;
