-- Merges of branches made by copying whose definitions changed: objects
-- merged one by one, as the side that changed each has it; then rows.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
CREATE TABLE app.u (id int PRIMARY KEY, w text);
CREATE TABLE app.d (x int);
INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 5) g;
INSERT INTO app.u SELECT g, 'w' || g FROM generate_series(1, 5) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');

-- feature: a new table, routine and view, a column, a dropped table
SELECT dolt_checkout('feature');
CREATE TABLE app.n (id int PRIMARY KEY, t int REFERENCES app.t);
INSERT INTO app.n VALUES (1, 1), (2, 2);
CREATE FUNCTION app.twice(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 2';
ALTER TABLE app.t ADD COLUMN extra int DEFAULT 0;
UPDATE app.t SET extra = id;
CREATE VIEW app.tv AS SELECT id, app.twice(extra) AS e FROM app.t;
DROP TABLE app.d;
CREATE TABLE app.same (k int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'feature defines')::text) AS committed;

-- main: rows of another table, and the same table made alike
SELECT dolt_checkout('main');
UPDATE app.u SET w = 'main' WHERE id = 1;
INSERT INTO app.u VALUES (6, 'six');
CREATE TABLE app.same (k int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'main rows')::text) AS committed;

SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_hashof('HEAD^2') = dolt_hashof('feature') AS merge_commit;
SELECT * FROM dolt_status();
SELECT * FROM app.t ORDER BY id;
SELECT * FROM app.tv ORDER BY id;
SELECT * FROM app.n ORDER BY id;
SELECT * FROM app.u ORDER BY id;
SELECT to_regclass('app.d') IS NULL AS d_dropped, to_regclass('app.same') IS NOT NULL AS same_made;
INSERT INTO app.n VALUES (3, 99);		-- its foreign key came too
-- the merge's root is the branch folded
SELECT test_fold_root() = prolly_root_hash('HEAD') AS same_root;

-- An object both sides changed differently: a schema conflict.
SELECT dolt_checkout('feature');
SELECT (dolt_merge('main'))[2:4];		-- feature catches up first
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 1';
SELECT length(dolt_commit('-Am', 'feature f')::text) AS committed;
SELECT dolt_checkout('main');
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 2';
SELECT length(dolt_commit('-Am', 'main f')::text) AS committed;
SELECT dolt_merge('feature');
SELECT to_regprocedure('app.f()') IS NOT NULL AS ours_kept, app.f();

-- A table redefined on one side, its rows changed on the other: a column
-- added empty leaves the rows as they were, so they merge.
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'HEAD~1');
ALTER TABLE app.u ADD COLUMN more int;
SELECT length(dolt_commit('-Am', 'feature redefines u')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.u SET w = 'main again' WHERE id = 2;
SELECT length(dolt_commit('-Am', 'main changes u')::text) AS committed;
SELECT (dolt_merge('feature'))[2:4];
SELECT * FROM app.u WHERE id <= 2 ORDER BY id;
SELECT * FROM dolt_status();
-- One with a default changes every row: the rows are merged as the
-- merged definition has them.
SELECT dolt_checkout('feature');
SELECT (dolt_merge('main'))[2:4];
ALTER TABLE app.u ADD COLUMN most int DEFAULT 5;
SELECT length(dolt_commit('-Am', 'feature fills u')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.u SET w = 'main once more' WHERE id = 3;
SELECT length(dolt_commit('-Am', 'main changes u again')::text) AS committed;
SELECT (dolt_merge('feature'))[2:4];
SELECT * FROM app.u WHERE id <= 3 ORDER BY id;
SELECT * FROM dolt_status();

SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'feature');

-- Versioned schemas merge as objects do: one side's new ones (empty too)
-- come, and one it dropped goes.
CREATE SCHEMA sg_gone VERSIONED;
SELECT length(dolt_commit('-Am', 'a schema to drop')::text) AS committed;
SELECT dolt_branch('sg_side');
SELECT dolt_checkout('sg_side') IS NOT NULL AS on_side;
CREATE SCHEMA sg_new VERSIONED;
CREATE TABLE sg_new.u (id int PRIMARY KEY);
CREATE SCHEMA sg_new_empty VERSIONED;
DROP SCHEMA sg_gone;
SELECT length(dolt_commit('-Am', 'schemas on the side')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
CREATE TABLE app.sg_main (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'a table on main')::text) AS committed;
SELECT (dolt_merge('sg_side'))[2:4];
SELECT nspname FROM pg_namespace WHERE nspname LIKE 'sg\_%' ORDER BY 1;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;
SELECT dolt_branch('-D', 'sg_side');

-- A table theirs made again (an identity added) keeps ours' rows.
CREATE SCHEMA mr VERSIONED;
CREATE TABLE mr.t (id int NOT NULL PRIMARY KEY, v text);
INSERT INTO mr.t VALUES (1, 'a'), (2, 'b');
CREATE TABLE mr.u (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'mr base')::text) AS committed;
SELECT dolt_branch('mr_side');
SELECT dolt_checkout('mr_side') IS NOT NULL AS on_side;
ALTER TABLE mr.t ALTER COLUMN id ADD GENERATED ALWAYS AS IDENTITY;
SELECT length(dolt_commit('-Am', 'identity')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO mr.u VALUES (1);
SELECT length(dolt_commit('-Am', 'mr main')::text) AS committed;
SELECT (dolt_merge('mr_side'))[2:4];
SELECT * FROM mr.t ORDER BY id;

-- A table both sides made alike merges its rows three-way.
SELECT dolt_branch('-D', 'mr_side');
SELECT dolt_branch('mr_side');
SELECT dolt_checkout('mr_side') IS NOT NULL AS on_side;
CREATE TABLE mr.both (k int PRIMARY KEY);
INSERT INTO mr.both VALUES (2);
SELECT length(dolt_commit('-Am', 'side makes both')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
CREATE TABLE mr.both (k int PRIMARY KEY);
INSERT INTO mr.both VALUES (1);
SELECT length(dolt_commit('-Am', 'main makes both')::text) AS committed;
SELECT (dolt_merge('mr_side'))[2:4];
SELECT * FROM mr.both ORDER BY k;

-- The other side's rows, taken into a table one side redefined, must fit
-- it as it is defined now: ours narrows a column, theirs adds a row too
-- long for it.
SELECT dolt_branch('-D', 'mr_side');
SELECT dolt_branch('mr_side');
SELECT dolt_checkout('mr_side') IS NOT NULL AS on_side;
INSERT INTO mr.u VALUES (2);
INSERT INTO mr.t OVERRIDING SYSTEM VALUE VALUES (3, 'abcdefghij');
SELECT length(dolt_commit('-Am', 'a long one')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
ALTER TABLE mr.t ALTER COLUMN v TYPE varchar(5);
SELECT length(dolt_commit('-Am', 'narrowed')::text) AS committed;
SELECT (dolt_merge('mr_side'))[2:4];
SELECT max(length(v)) FROM mr.t;

-- A sequence ours dropped, whose value theirs changed: dropped.
SELECT dolt_branch('-D', 'mr_side');
CREATE SEQUENCE mr.s;
SELECT length(dolt_commit('-Am', 'mr.s')::text) AS committed;
SELECT dolt_branch('mr_side');
SELECT dolt_checkout('mr_side') IS NOT NULL AS on_side;
SELECT nextval('mr.s');
SELECT length(dolt_commit('-Am', 'side takes a value')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
DROP SEQUENCE mr.s;
SELECT length(dolt_commit('-Am', 'main drops it')::text) AS committed;
SELECT (dolt_merge('mr_side'))[2:4];
SELECT to_regclass('mr.s') IS NULL AS dropped;

-- What one side dropped that the other side's new objects use is a schema
-- conflict: a schema with a new table in it, a function a new view calls.
SELECT dolt_branch('-D', 'mr_side');
CREATE SCHEMA mr_gone VERSIONED;
CREATE FUNCTION mr.twice(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 2';
SELECT length(dolt_commit('-Am', 'mr_gone, twice')::text) AS committed;
SELECT dolt_branch('mr_side');
SELECT dolt_checkout('mr_side') IS NOT NULL AS on_side;
DROP SCHEMA mr_gone;
SELECT length(dolt_commit('-Am', 'side drops mr_gone')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
CREATE TABLE mr_gone.x (id int PRIMARY KEY);
INSERT INTO mr_gone.x VALUES (1);
SELECT length(dolt_commit('-Am', 'main fills mr_gone')::text) AS committed;
SELECT dolt_merge('mr_side');
SELECT * FROM mr_gone.x;
SELECT dolt_branch('-D', 'mr_side');
DROP SCHEMA mr_gone CASCADE;
SELECT length(dolt_commit('-Am', 'without mr_gone')::text) AS committed;
SELECT dolt_branch('mr_side');
SELECT dolt_checkout('mr_side') IS NOT NULL AS on_side;
DROP FUNCTION mr.twice(int);
SELECT length(dolt_commit('-Am', 'side drops twice')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
CREATE VIEW mr.tv AS SELECT mr.twice(id) AS d FROM mr.u;
SELECT length(dolt_commit('-Am', 'main uses twice')::text) AS committed;
SELECT dolt_merge('mr_side');
SELECT dolt_branch('-D', 'mr_side');
DROP SCHEMA mr CASCADE;
SELECT length(dolt_commit('-Am', 'without mr')::text) AS committed;
-- a routine with several arguments kept by a merge (its signature,
-- "ma.add(integer, integer)", isn't a qualified name)
CREATE SCHEMA ma VERSIONED;
CREATE FUNCTION ma.add(int, int) RETURNS int LANGUAGE sql AS 'SELECT $1 + $2';
CREATE TABLE ma.t (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'ma')::text) AS committed;
SELECT dolt_checkout('-b', 'ma_side') IS NOT NULL AS on_side;
CREATE TABLE ma.s (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'side table')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
CREATE TABLE ma.m (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'main table')::text) AS committed;
SELECT dolt_merge('ma_side') IS NOT NULL AS merged;
SELECT ma.add(1, 2);
SELECT dolt_branch('-D', 'ma_side');
-- dropped on one side, given such a routine on the other: a conflict
SELECT dolt_checkout('-b', 'ma_drop') IS NOT NULL AS on_drop;
DROP SCHEMA ma CASCADE;
SELECT length(dolt_commit('-Am', 'ma dropped')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
CREATE FUNCTION ma.mul(int, int, int) RETURNS int LANGUAGE sql AS 'SELECT $1 * $2 * $3';
SELECT length(dolt_commit('-Am', 'ma.mul')::text) AS committed;
SELECT dolt_merge('ma_drop');
SELECT dolt_branch('-D', 'ma_drop');
DROP SCHEMA ma CASCADE;
SELECT length(dolt_commit('-Am', 'without ma')::text) AS committed;

-- a versioned public dropped on one side: what has no schema (the root's
-- format) isn't taken for public's
SELECT current_database() AS db \gset
CREATE DATABASE regress_public_merge;
\c regress_public_merge
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE TABLE public.t (id int PRIMARY KEY);
CREATE SCHEMA other VERSIONED;
CREATE TABLE other.t (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'public')::text) AS committed;
SELECT dolt_checkout('-b', 'no_public') IS NOT NULL AS on_side;
DROP SCHEMA public CASCADE;
SELECT length(dolt_commit('-Am', 'public dropped')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO other.t VALUES (1);
SELECT length(dolt_commit('-Am', 'a row elsewhere')::text) AS committed;
SELECT dolt_merge('no_public') IS NOT NULL AS merged;
SELECT count(*) AS public_schemas FROM pg_namespace WHERE nspname = 'public';
\c :db
DROP DATABASE regress_public_merge;
DROP SCHEMA sg_new CASCADE;
DROP SCHEMA sg_new_empty;

-- A table keyed by content though it has a primary key (one over a varchar
-- column) merges across a column added on one side: its key didn't change.
CREATE SCHEMA svk VERSIONED;
CREATE TABLE svk.t (pk1 int, col1 int, pk2 varchar(100), CHECK (col1 IN (0, 1)), PRIMARY KEY (pk1, pk2));
INSERT INTO svk.t VALUES (1, 0, '1');
SELECT length(dolt_commit('-Am', 'svk base')::text) AS committed;
SELECT dolt_branch('svk_right');
SELECT dolt_checkout('svk_right');
ALTER TABLE svk.t ADD COLUMN col2 varchar(100);
INSERT INTO svk.t VALUES (2, 1, '2', 'hello');
SELECT length(dolt_commit('-Am', 'svk right')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO svk.t VALUES (3, 0, '3');
SELECT length(dolt_commit('-Am', 'svk left')::text) AS committed;
SELECT (dolt_merge('svk_right'))[2:4];
SELECT * FROM svk.t ORDER BY pk1;
SELECT dolt_branch('-D', 'svk_right');
DROP SCHEMA svk CASCADE;

-- A generated column added on one side: the other side's rows get it,
-- computed (stored) or not stored at all (virtual).
CREATE SCHEMA sgc VERSIONED;
CREATE TABLE sgc.t (pk int PRIMARY KEY);
INSERT INTO sgc.t VALUES (1);
SELECT length(dolt_commit('-Am', 'sgc base')::text) AS committed;
SELECT dolt_branch('sgc_right');
SELECT dolt_checkout('sgc_right');
ALTER TABLE sgc.t ADD COLUMN s int GENERATED ALWAYS AS (pk * 10) STORED,
	ADD COLUMN v int GENERATED ALWAYS AS (pk + 1);
INSERT INTO sgc.t VALUES (3);
SELECT length(dolt_commit('-Am', 'sgc right')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO sgc.t VALUES (2);
SELECT length(dolt_commit('-Am', 'sgc left')::text) AS committed;
SELECT (dolt_merge('sgc_right'))[2:4];
SELECT * FROM sgc.t ORDER BY pk;
SELECT pk FROM sgc.t WHERE s = 20;
SELECT dolt_branch('-D', 'sgc_right');
DROP SCHEMA sgc CASCADE;

-- A table both sides changed merges part by part: each side's columns,
-- constraints and indexes, the other's rows given the columns they lack;
-- a part both changed differently is a schema conflict.
CREATE SCHEMA sbt VERSIONED;
CREATE TABLE sbt.parent (id int PRIMARY KEY);
INSERT INTO sbt.parent VALUES (1);
CREATE TABLE sbt.t (pk int PRIMARY KEY, a int, gone int, p int);
INSERT INTO sbt.t VALUES (1, 1, 1, 1);
SELECT length(dolt_commit('-Am', 'sbt base')::text) AS committed;
SELECT dolt_branch('sbt_right');
SELECT dolt_checkout('sbt_right');
ALTER TABLE sbt.t ADD COLUMN r int DEFAULT 7, ADD CONSTRAINT a_pos CHECK (a > 0);
CREATE INDEX t_r ON sbt.t (r);
INSERT INTO sbt.t VALUES (2, 2, 2, 1, 20);
SELECT length(dolt_commit('-Am', 'sbt right')::text) AS committed;
SELECT dolt_checkout('main');
ALTER TABLE sbt.t ADD COLUMN l text, DROP COLUMN gone,
	ADD CONSTRAINT t_p_fk FOREIGN KEY (p) REFERENCES sbt.parent (id);
INSERT INTO sbt.t VALUES (3, 3, 1, 'left');
UPDATE sbt.t SET l = 'one' WHERE pk = 1;
SELECT length(dolt_commit('-Am', 'sbt left')::text) AS committed;
SELECT (dolt_merge('sbt_right'))[2:4];
SELECT * FROM sbt.t ORDER BY pk;
SELECT conname FROM pg_constraint WHERE conrelid = 'sbt.t'::regclass ORDER BY 1;
SELECT indexname FROM pg_indexes WHERE schemaname = 'sbt' AND tablename = 't' ORDER BY 1;
-- both change one column differently: a conflict
SELECT dolt_branch('sbt_other');
ALTER TABLE sbt.t ALTER COLUMN a TYPE bigint;
SELECT length(dolt_commit('-am', 'a bigint')::text) AS committed;
SELECT dolt_checkout('sbt_other');
ALTER TABLE sbt.t ALTER COLUMN a TYPE numeric;
SELECT length(dolt_commit('-am', 'a numeric')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('sbt_other');
-- a foreign key added on one side that the other's rows break: added,
-- the rows kept and recorded
SELECT dolt_branch('sbt_fk');
INSERT INTO sbt.t (pk, a, p) VALUES (4, 4, 1);
SELECT length(dolt_commit('-am', 'row 4')::text) AS committed;
SELECT dolt_checkout('sbt_fk');
DELETE FROM sbt.t WHERE a <> 1;
ALTER TABLE sbt.t ADD CONSTRAINT t_a_fk FOREIGN KEY (a) REFERENCES sbt.parent (id);
SELECT length(dolt_commit('-am', 'a fk')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('sbt_fk'))[2:4];
SELECT pk, a FROM sbt.t ORDER BY pk;
SELECT violation_type, violating_row->>'pk' AS pk FROM dolt_constraint_violations('sbt.t') ORDER BY 2;
SELECT convalidated FROM pg_constraint WHERE conname = 't_a_fk';
ROLLBACK;
SELECT dolt_branch('-D', 'sbt_right');
SELECT dolt_branch('-D', 'sbt_other');
SELECT dolt_branch('-D', 'sbt_fk');
DROP SCHEMA sbt CASCADE;

-- A check and a NOT NULL added on one side that the other side's rows
-- break: those rows are taken out of the table and recorded as violations,
-- and the constraints hold, validated.
CREATE SCHEMA snn VERSIONED;
CREATE TABLE snn.t (pk int PRIMARY KEY, a int, b int);
INSERT INTO snn.t VALUES (1, NULL, 1), (2, NULL, 1);
SELECT length(dolt_commit('-Am', 'snn base')::text) AS committed;
SELECT dolt_branch('snn_right');
SELECT dolt_checkout('snn_right');
UPDATE snn.t SET a = 0;
ALTER TABLE snn.t ALTER COLUMN a SET NOT NULL, ADD CONSTRAINT b_small CHECK (b < 5);
SELECT length(dolt_commit('-Am', 'snn right')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO snn.t VALUES (3, NULL, 1), (4, 4, 9), (5, 5, 1);
SELECT length(dolt_commit('-Am', 'snn left')::text) AS committed;
BEGIN;
SELECT (dolt_merge('snn_right'))[2:4];
SELECT * FROM snn.t ORDER BY pk;
SELECT violation_type, violating_row, violation_info
  FROM dolt_constraint_violations('snn.t') ORDER BY violating_row->>'pk';
SELECT conname, convalidated FROM pg_constraint
  WHERE conrelid = 'snn.t'::regclass AND contype IN ('c', 'n') ORDER BY conname;
ROLLBACK;
SELECT dolt_branch('-D', 'snn_right');
DROP SCHEMA snn CASCADE;

-- A row taken out for breaking a check the merge added, which another
-- table's rows reference: those rows stay, recorded as breaking their
-- foreign key (no trigger fires for the row taken out).
CREATE SCHEMA sfk VERSIONED;
CREATE TABLE sfk.parent (id int PRIMARY KEY, v int);
CREATE TABLE sfk.child (id int PRIMARY KEY, pid int REFERENCES sfk.parent ON DELETE CASCADE);
INSERT INTO sfk.parent VALUES (1, 1), (2, 2);
INSERT INTO sfk.child VALUES (10, 1), (20, 2);
SELECT length(dolt_commit('-Am', 'sfk base')::text) AS committed;
SELECT dolt_branch('sfk_right');
SELECT dolt_checkout('sfk_right');
ALTER TABLE sfk.parent ADD CONSTRAINT v_pos CHECK (v > 0);
SELECT length(dolt_commit('-Am', 'sfk right')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE sfk.parent SET v = -1 WHERE id = 1;
SELECT length(dolt_commit('-Am', 'sfk left')::text) AS committed;
BEGIN;
SELECT (dolt_merge('sfk_right'))[2:4];
SELECT * FROM sfk.parent ORDER BY id;
SELECT * FROM sfk.child ORDER BY id;
SELECT 'parent' AS t, violation_type, violating_row FROM dolt_constraint_violations('sfk.parent')
UNION ALL
SELECT 'child', violation_type, violating_row FROM dolt_constraint_violations('sfk.child')
ORDER BY 1;
ROLLBACK;
SELECT dolt_branch('-D', 'sfk_right');
DROP SCHEMA sfk CASCADE;

-- Two sides' primary keys, or NOT NULLs of one column, named differently:
-- a table can't have both, a schema conflict.
CREATE SCHEMA spk VERSIONED;
CREATE TABLE spk.t (a int, b int, c int);
SELECT length(dolt_commit('-Am', 'spk base')::text) AS committed;
SELECT dolt_branch('spk_right');
SELECT dolt_branch('spk_nn');
SELECT dolt_checkout('spk_right');
ALTER TABLE spk.t ADD CONSTRAINT pk2 PRIMARY KEY (b);
SELECT length(dolt_commit('-Am', 'spk right')::text) AS committed;
SELECT dolt_checkout('spk_nn');
ALTER TABLE spk.t ADD CONSTRAINT c_set NOT NULL c;
SELECT length(dolt_commit('-Am', 'spk nn')::text) AS committed;
SELECT dolt_checkout('main');
ALTER TABLE spk.t ADD PRIMARY KEY (a), ALTER COLUMN c SET NOT NULL;
SELECT length(dolt_commit('-Am', 'spk left')::text) AS committed;
SELECT dolt_merge('spk_right');
SELECT dolt_merge('spk_nn');
SELECT dolt_branch('-D', 'spk_right');
SELECT dolt_branch('-D', 'spk_nn');
DROP SCHEMA spk CASCADE;

-- A foreign key added referencing a partitioned table: validated, as are
-- the constraints Postgres made for it for each partition.
CREATE SCHEMA sfp VERSIONED;
CREATE TABLE sfp.parted (id int PRIMARY KEY) PARTITION BY RANGE (id);
CREATE TABLE sfp.parted_1 PARTITION OF sfp.parted FOR VALUES FROM (0) TO (100);
CREATE TABLE sfp.parted_2 PARTITION OF sfp.parted FOR VALUES FROM (100) TO (200);
INSERT INTO sfp.parted VALUES (1), (150);
CREATE TABLE sfp.t (id int PRIMARY KEY, x int);
INSERT INTO sfp.t VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'sfp base')::text) AS committed;
SELECT dolt_branch('sfp_right');
SELECT dolt_checkout('sfp_right');
ALTER TABLE sfp.t ADD CONSTRAINT t_x_fk FOREIGN KEY (x) REFERENCES sfp.parted;
SELECT length(dolt_commit('-Am', 'sfp right')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO sfp.t VALUES (2, 150), (3, 999);
SELECT length(dolt_commit('-Am', 'sfp left')::text) AS committed;
BEGIN;
SELECT (dolt_merge('sfp_right'))[2:4];
SELECT conrelid::regclass, confrelid::regclass, convalidated FROM pg_constraint
  WHERE conname LIKE 't_x_fk%' ORDER BY 2::text;
SELECT violation_type, violating_row FROM dolt_constraint_violations('sfp.t');
ROLLBACK;
SELECT dolt_branch('-D', 'sfp_right');
DROP SCHEMA sfp CASCADE;

-- A key over a column named in quotes, added on one side: moved and merged.
CREATE SCHEMA sqk VERSIONED;
CREATE TABLE sqk.t ("MyCol" int, v int);
INSERT INTO sqk.t VALUES (1, 1), (2, 2);
SELECT length(dolt_commit('-Am', 'sqk base')::text) AS committed;
SELECT dolt_branch('sqk_right');
SELECT dolt_checkout('sqk_right');
ALTER TABLE sqk.t ADD PRIMARY KEY ("MyCol");
SELECT length(dolt_commit('-Am', 'sqk right')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_checkout('sqk_right');
SELECT conname, contype, convalidated FROM pg_constraint
  WHERE conrelid = 'sqk.t'::regclass ORDER BY 1;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'sqk_right');
DROP SCHEMA sqk CASCADE;

-- A generated column's expression changed on one side, rows written on
-- the other: their values computed by the expression as merged.
CREATE SCHEMA sge VERSIONED;
CREATE TABLE sge.t (pk int PRIMARY KEY, a int, g int GENERATED ALWAYS AS (a * 2) STORED);
INSERT INTO sge.t (pk, a) VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'sge base')::text) AS committed;
SELECT dolt_branch('sge_right');
SELECT dolt_checkout('sge_right');
ALTER TABLE sge.t ALTER COLUMN g SET EXPRESSION AS (a * 3);
SELECT length(dolt_commit('-Am', 'sge right')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO sge.t (pk, a) VALUES (2, 2);
SELECT length(dolt_commit('-Am', 'sge left')::text) AS committed;
SELECT (dolt_merge('sge_right'))[2:4];
SELECT * FROM sge.t ORDER BY pk;
SELECT dolt_branch('-D', 'sge_right');
DROP SCHEMA sge CASCADE;

-- A column one side dropped and the other changed, the table's key
-- dropped too: refused for the key, not misread.
CREATE SCHEMA slk VERSIONED;
CREATE TABLE slk.t (pk int PRIMARY KEY, a int, c int);
INSERT INTO slk.t VALUES (1, 1, 1);
SELECT length(dolt_commit('-Am', 'slk base')::text) AS committed;
SELECT dolt_branch('slk_right');
SELECT dolt_checkout('slk_right');
UPDATE slk.t SET c = 2;
SELECT length(dolt_commit('-Am', 'slk right')::text) AS committed;
SELECT dolt_checkout('main');
ALTER TABLE slk.t DROP CONSTRAINT t_pkey, DROP COLUMN c;
SELECT length(dolt_commit('-Am', 'slk left')::text) AS committed;
SELECT dolt_merge('slk_right');
SELECT dolt_branch('-D', 'slk_right');
DROP SCHEMA slk CASCADE;
DROP SCHEMA app CASCADE;
