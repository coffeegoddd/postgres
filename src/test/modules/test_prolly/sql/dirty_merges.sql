-- Merges into a working set with uncommitted changes, as in Dolt: allowed
-- when the merge changes none of the objects they changed (tables,
-- sequences, and with definitions changing, any); they stay uncommitted,
-- left out of the merge's commit.

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
-- not when the merge changes that table's rows
ALTER TABLE dm.a ADD COLUMN w int;
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_reset('--hard');
-- A merge that changes definitions goes ahead too (#494): the
-- uncommitted changes are set aside, the merge made as over a clean
-- working set, and they're put back, uncommitted.  One altering a
-- table, over a table made:
SELECT dolt_checkout('feature');
ALTER TABLE dm.a ADD COLUMN x int;
SELECT length(dolt_commit('-am', 'feature alters a')::text) AS committed;
SELECT dolt_checkout('main');
CREATE TABLE dm.made (id int PRIMARY KEY);
INSERT INTO dm.made VALUES (1);
SELECT (dolt_merge('feature'))[2:4];
SELECT column_name FROM information_schema.columns
  WHERE table_schema = 'dm' AND table_name = 'a' ORDER BY ordinal_position;
SELECT * FROM dm.made;
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_checkout('main');
-- three-way, making a table, over uncommitted rows of another, which the
-- merge's commit leaves out
SELECT dolt_checkout('feature');
CREATE TABLE dm.theirs_made (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'feature makes a table')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE dm.b SET v = 'dirty' WHERE id = 5;
SELECT (dolt_merge('feature', '--no-ff', '-m', 'made'))[3:4];
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT id, v FROM dm.b WHERE id = 5;
SELECT count(*) AS b_left_out FROM dolt_diff('HEAD~1', 'HEAD', NULL::dm.b);
SELECT to_regclass('dm.theirs_made') IS NOT NULL AS theirs_made;
SELECT dolt_reset('--hard', 'HEAD~1');
-- over uncommitted definitions of other kinds (a view, a type), which stay
CREATE VIEW dm.v AS SELECT id FROM dm.b;
CREATE TYPE dm.local_t AS ENUM ('x');
SELECT (dolt_merge('feature', '--no-ff', '-m', 'made'))[3:4];
SELECT to_regclass('dm.theirs_made') IS NOT NULL AS theirs_made,
  to_regclass('dm.v') IS NOT NULL AS view_kept,
  to_regtype('dm.local_t') IS NOT NULL AS type_kept;
SELECT from_table_name, to_table_name, diff_type
  FROM dolt_diff_summary('HEAD~1', 'HEAD') ORDER BY 2;
SELECT to_table_name, diff_type
  FROM dolt_diff_summary('HEAD', 'WORKING') ORDER BY 1;
SELECT dolt_reset('--hard');
SELECT to_regclass('dm.v') IS NULL AS view_gone;
SELECT dolt_reset('--hard', 'HEAD~1');
-- refused where both change an object: here, both make the table
CREATE TABLE dm.theirs_made (id int PRIMARY KEY, mine text);
SELECT (dolt_merge('feature', '--no-ff', '-m', 'made'))[3:4];
SELECT count(*) AS mine FROM information_schema.columns
  WHERE table_schema = 'dm' AND table_name = 'theirs_made' AND column_name = 'mine';
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
CREATE MATERIALIZED VIEW dms.mv AS SELECT v FROM dms.t;
SELECT length(dolt_commit('-Am', 'dms base')::text) AS committed;
SELECT dolt_branch('dms_feature');
SELECT dolt_checkout('dms_feature');
UPDATE dms.t SET v = 'feature' WHERE id = 1;
REFRESH MATERIALIZED VIEW dms.mv;
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
SELECT v FROM dms.mv;
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

-- Merges, picks and reverts changing definitions over uncommitted
-- changes (#494): a local ALTER TABLE of a table the merge doesn't
-- touch survives; refused where the local changes use what it changed
-- (a type, here), break what it added (a foreign key), or don't fit it.
CREATE SCHEMA dmd VERSIONED;
CREATE TYPE dmd.mood AS ENUM ('sad', 'ok');
CREATE TABLE dmd.m (id int PRIMARY KEY, m dmd.mood);
CREATE TABLE dmd.p (id int PRIMARY KEY);
CREATE TABLE dmd.c (id int PRIMARY KEY, pid int);
CREATE TABLE dmd.u (id int PRIMARY KEY, v text);
INSERT INTO dmd.m VALUES (1, 'ok');
INSERT INTO dmd.p VALUES (1), (2);
INSERT INTO dmd.u VALUES (1, 'u');
SELECT length(dolt_commit('-Am', 'dmd base')::text) AS committed;
SELECT dolt_branch('dmd_feature');
SELECT dolt_checkout('dmd_feature');
ALTER TYPE dmd.mood ADD VALUE 'happy';
ALTER TABLE dmd.c ADD FOREIGN KEY (pid) REFERENCES dmd.p;
INSERT INTO dmd.c VALUES (1, 1);
SELECT length(dolt_commit('-am', 'feature redefines')::text) AS committed;
SELECT dolt_checkout('main');
-- a fast-forward
ALTER TABLE dmd.u ADD COLUMN w int;
SELECT (dolt_merge('dmd_feature'))[2:4];
SELECT dolt_hashof('HEAD') = dolt_hashof('dmd_feature') AS fast_forwarded;
SELECT count(*) AS c_rows FROM dmd.c;
SELECT column_name FROM information_schema.columns
  WHERE table_schema = 'dmd' AND table_name = 'u' ORDER BY ordinal_position;
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT dolt_reset('--hard', 'HEAD~1');
-- three-way
INSERT INTO dmd.u VALUES (2, 'side');
SELECT length(dolt_commit('-am', 'main adds a row')::text) AS committed;

ALTER TABLE dmd.u ADD COLUMN w int;
UPDATE dmd.u SET w = 7 WHERE id = 1;
SELECT (dolt_merge('dmd_feature', '-m', 'merged over an alter'))[3:4];
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT * FROM dmd.u ORDER BY id;
SELECT * FROM dmd.c;
SELECT unnest(enum_range(NULL::dmd.mood)) AS label;
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT count(*) AS u_left_out FROM dolt_diff('HEAD~1', 'HEAD', NULL::dmd.u);
SELECT dolt_reset('--hard', 'HEAD~1');

-- a table using the type the merge changes
UPDATE dmd.m SET m = 'sad';
SELECT (dolt_merge('dmd_feature'))[3:4];
SELECT dolt_reset('--hard');
-- a row the merge's new foreign key references, removed
DELETE FROM dmd.p WHERE id = 1;
SELECT (dolt_merge('dmd_feature'))[3:4];
SELECT id FROM dmd.p ORDER BY id;
SELECT dolt_reset('--hard');
-- the key that foreign key needs, dropped
ALTER TABLE dmd.p DROP CONSTRAINT p_pkey;
SELECT (dolt_merge('dmd_feature'))[3:4];
SELECT dolt_reset('--hard');
SELECT dolt_hashof('HEAD') <> dolt_hashof('dmd_feature') AS not_merged;

-- a cherry-pick, then its revert, over the alter too
ALTER TABLE dmd.u ADD COLUMN w int;
SELECT length(dolt_cherry_pick('dmd_feature')::text) > 0 AS picked;
SELECT count(*) AS c_rows FROM dmd.c;
SELECT column_name FROM information_schema.columns
  WHERE table_schema = 'dmd' AND table_name = 'u' ORDER BY ordinal_position;
SELECT length(dolt_revert('HEAD')::text) > 0 AS reverted;
SELECT count(*) AS c_rows FROM dmd.c;
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT count(*) AS u_left_out FROM dolt_diff('HEAD~2', 'HEAD', NULL::dmd.u);
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'dmd_feature');
DROP SCHEMA dmd CASCADE;
SELECT length(dolt_commit('-Am', 'dmd gone')::text) AS committed;
