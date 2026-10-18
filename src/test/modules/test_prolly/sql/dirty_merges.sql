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

-- A merge that conflicts isn't made over uncommitted changes: its commit
-- would take them in, and its abort (a reset) would lose them.
SELECT dolt_checkout('feature');
UPDATE dm.a SET v = 'theirs' WHERE id = 4;
SELECT length(dolt_commit('-am', 'feature changes 4')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE dm.a SET v = 'ours' WHERE id = 4;
SELECT length(dolt_commit('-am', 'main changes 4')::text) AS committed;
UPDATE dm.b SET v = 'kept' WHERE id = 3;
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
ROLLBACK;
SELECT id, v FROM dm.b WHERE id = 3;
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT dolt_reset('--hard');

-- A commit made as a merge's is (with a parent given) checks the parent
-- is still the head, as any commit does.
SELECT test_commit_with('HEAD~1');
SELECT test_commit_with('HEAD') IS NOT NULL AS committed_on_the_head;

-- A materialized view refreshed is a change of its rows, not of its
-- definition: a merge that doesn't refresh it goes ahead and leaves it
-- uncommitted; one that does is refused.
CREATE MATERIALIZED VIEW dm.mv AS SELECT count(*) AS n FROM dm.a;
SELECT length(dolt_commit('-Am', 'a view')::text) AS committed;
SELECT dolt_branch('-D', 'feature');
SELECT dolt_branch('feature');
SELECT dolt_checkout('feature');
UPDATE dm.b SET v = 'feature' WHERE id = 5;
SELECT length(dolt_commit('-am', 'feature changes b')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO dm.a VALUES (6, 'a6');
SELECT length(dolt_commit('-am', 'main adds a row')::text) AS committed;
SELECT md5(value) AS mv_head FROM test_fold_items() WHERE key = 'm:dm.mv' \gset
REFRESH MATERIALIZED VIEW dm.mv;
SELECT md5(value) <> :'mv_head' AS refreshed FROM test_fold_items() WHERE key = 'm:dm.mv';
SELECT (dolt_merge('feature', '-m', 'merged over a refresh'))[3:4];
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT n FROM dm.mv;
SELECT md5(value) <> :'mv_head' AS still_refreshed FROM test_fold_items() WHERE key = 'm:dm.mv';
SELECT dolt_reset('--hard');
-- (the merge's commit has the view as the head had it)
SELECT md5(value) = :'mv_head' AS refresh_left_out FROM test_fold_items() WHERE key = 'm:dm.mv';
SELECT dolt_checkout('feature');
INSERT INTO dm.a VALUES (7, 'a7');
REFRESH MATERIALIZED VIEW dm.mv;
SELECT length(dolt_commit('-am', 'feature refreshes it')::text) AS committed;
SELECT dolt_checkout('main');
REFRESH MATERIALIZED VIEW dm.mv;
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_reset('--hard');

-- A collation (of an ordinary schema) that orders otherwise now isn't an
-- uncommitted change the merge must leave out: its commit records the
-- collation as it is.
CREATE COLLATION public.dm_c_utf8 (provider = builtin, locale = 'C.UTF-8');
CREATE TABLE dm.k (k text COLLATE public.dm_c_utf8 PRIMARY KEY);
INSERT INTO dm.k VALUES ('a'), ('b');
SELECT length(dolt_commit('-Am', 'a collated table')::text) AS committed;
SELECT dolt_checkout('feature');
UPDATE dm.b SET v = 'again' WHERE id = 5;
SELECT length(dolt_commit('-am', 'feature changes b again')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE pg_collation SET collicurules = '&a<b' WHERE collname = 'dm_c_utf8';
SELECT (dolt_merge('feature', '-m', 'merged with a collation changed'))[3:4];
SELECT table_name, status FROM dolt_status ORDER BY 1;
UPDATE pg_collation SET collicurules = NULL WHERE collname = 'dm_c_utf8';

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA dm CASCADE;
DROP COLLATION public.dm_c_utf8;
