-- Merges into a working set with uncommitted changes, as in Dolt: allowed
-- when the merge changes none of the tables and sequences they changed,
-- and no definitions change on both sides; they stay uncommitted, left
-- out of the merge's commit.

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

-- Uncommitted changes of tables' definitions are merged into when the
-- merge changes neither those tables nor any definition: a table altered
-- or made stays as it is, uncommitted.
SELECT dolt_reset('--hard');
ALTER TABLE dm.b ADD COLUMN w int;
CREATE TABLE dm.made (id int PRIMARY KEY);
SELECT (dolt_merge('feature'))[2:4];
SELECT count(*) AS b_left_out FROM dolt_diff('HEAD~1', 'HEAD', NULL::dm.b);
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT column_name FROM information_schema.columns
  WHERE table_schema = 'dm' AND table_name = 'b' ORDER BY ordinal_position;
SELECT dolt_reset('--hard', 'HEAD~1');
-- not when the merge changes that table's rows, nor when it changes
-- definitions too
ALTER TABLE dm.a ADD COLUMN w int;
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_reset('--hard');
SELECT dolt_checkout('feature');
ALTER TABLE dm.a ADD COLUMN x int;
SELECT length(dolt_commit('-am', 'feature alters a')::text) AS committed;
SELECT dolt_checkout('main');
CREATE TABLE dm.made (id int PRIMARY KEY);
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_reset('--hard');
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_checkout('main');
-- nor when it changes definitions over uncommitted rows of another table
-- (here, makes one: their table isn't the base's), which stay
SELECT dolt_checkout('feature');
CREATE TABLE dm.theirs_made (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'feature makes a table')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE dm.b SET v = 'dirty' WHERE id = 5;
SELECT (dolt_merge('feature', '--no-ff', '-m', 'made'))[3:4];
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT dolt_reset('--hard');
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_checkout('main');

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

-- A fast-forward over an uncommitted foreign key whose rows it would
-- break: refused, as a merge leaving violations over uncommitted changes
-- is.
CREATE SCHEMA dmf VERSIONED;
CREATE TABLE dmf.a (id int PRIMARY KEY);
CREATE TABLE dmf.b (id int PRIMARY KEY, aid int);
INSERT INTO dmf.a VALUES (1), (2);
INSERT INTO dmf.b VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'dmf base')::text) AS committed;
SELECT dolt_branch('dmf_feature');
SELECT dolt_checkout('dmf_feature');
DELETE FROM dmf.a WHERE id = 1;
SELECT length(dolt_commit('-am', 'a row gone')::text) AS committed;
SELECT dolt_checkout('main');
ALTER TABLE dmf.b ADD FOREIGN KEY (aid) REFERENCES dmf.a;
SELECT (dolt_merge('dmf_feature'))[2:4];
SELECT id FROM dmf.a ORDER BY id;
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'dmf_feature');
DROP SCHEMA dmf CASCADE;
SELECT length(dolt_commit('-Am', 'dmf gone')::text) AS committed;

-- A squashed fast-forward over changes to another table keeps them too,
-- as Dolt's does: theirs' rows and sequence values written, the head left
-- where it is, nothing committed.
CREATE SCHEMA dms VERSIONED;
CREATE TABLE dms.t (id int PRIMARY KEY, v text);
CREATE TABLE dms.u (id int PRIMARY KEY, v text);
CREATE SEQUENCE dms.s;
INSERT INTO dms.t VALUES (1, 'base');
INSERT INTO dms.u VALUES (1, 'base');
SELECT length(dolt_commit('-Am', 'dms base')::text) AS committed;
SELECT dolt_branch('dms_feature');
SELECT dolt_checkout('dms_feature');
UPDATE dms.t SET v = 'feature' WHERE id = 1;
SELECT nextval('dms.s');
SELECT nextval('dms.s');
SELECT length(dolt_commit('-am', 'feature changes t')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO dms.u VALUES (2, 'mine');
SELECT dolt_hashof('HEAD') AS dms_head \gset
SELECT (dolt_merge('--squash', 'dms_feature'))[2:4];
SELECT dolt_hashof('HEAD') = :'dms_head' AS head_kept;
SELECT id, v FROM dms.t;
SELECT id, v FROM dms.u ORDER BY id;
SELECT last_value, is_called FROM dms.s;
SELECT table_name, status FROM dolt_status ORDER BY 1;
-- over changes to a table the merge changes, it is refused
SELECT dolt_reset('--hard');
UPDATE dms.t SET v = 'mine' WHERE id = 1;
SELECT (dolt_merge('--squash', 'dms_feature'))[2:4];
SELECT id, v FROM dms.t;
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'dms_feature');
DROP SCHEMA dms CASCADE;
SELECT length(dolt_commit('-Am', 'dms gone')::text) AS committed;
