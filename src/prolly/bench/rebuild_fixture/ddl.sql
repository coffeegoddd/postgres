-- What each schema change does to stored rows and to the diff.  Each step
-- runs on the state the steps before it left; its expect lines must all be
-- in the diff against the step before (see ws_diff.c for the lines).
-- base
CREATE SCHEMA rt;
CREATE TABLE rt.t (id int PRIMARY KEY, v text, n int, z text);
INSERT INTO rt.t SELECT g, 'v' || g, g, NULL FROM generate_series(1, 5000) g;
CREATE TABLE rt.k (a int, b text);
INSERT INTO rt.k SELECT g % 100, 'b' || g % 7 FROM generate_series(1, 2000) g;
-- step rename a column
ALTER TABLE rt.t RENAME COLUMN v TO w;
-- expect
column renamed t.v w
data t same
rows t +0 -0 ~0
-- step add a nullable column
ALTER TABLE rt.t ADD COLUMN extra text;
-- expect
column added t.extra
data t same
-- step add a column with a default
ALTER TABLE rt.t ADD COLUMN flag int DEFAULT 7;
-- expect
column added t.flag
data t changed
rows t +0 -0 ~0
rewritten t 5000
-- step retype, the text unchanged
ALTER TABLE rt.t ALTER COLUMN n TYPE bigint;
-- expect
column retyped t.n integer -> bigint
data t same
-- step retype, the text changed
ALTER TABLE rt.t ALTER COLUMN n TYPE numeric(10,2);
-- expect
column retyped t.n bigint -> numeric(10,2)
data t changed
rows t +0 -0 ~5000
-- step drop a column that is all NULL
ALTER TABLE rt.t DROP COLUMN z;
-- expect
column dropped t.z
data t same
-- step drop a column with values
ALTER TABLE rt.t DROP COLUMN flag;
-- expect
column dropped t.flag
data t changed
rows t +0 -0 ~0
rewritten t 5000
-- step add an index
CREATE INDEX t_w ON rt.t (w);
-- expect
object added INDEX t_w
data t same
-- step change rows
UPDATE rt.t SET w = 'x' WHERE id <= 10;
DELETE FROM rt.t WHERE id > 4990;
INSERT INTO rt.t (id, w, n) VALUES (9001, 'new', 1);
-- expect
data t changed
rows t +1 -10 ~10
-- step drop a column and add it back
ALTER TABLE rt.t DROP COLUMN extra;
ALTER TABLE rt.t ADD COLUMN extra text;
-- expect
data t same
-- step keyless: add a nullable column
ALTER TABLE rt.k ADD COLUMN c int;
-- expect
column added k.c
data k same
-- step keyless: rename a column
ALTER TABLE rt.k RENAME COLUMN b TO bb;
-- expect
column renamed k.b bb
data k same
-- step keyless: add a column with a default
ALTER TABLE rt.k ADD COLUMN d int DEFAULT 1;
-- expect
data k changed
rows k +0 -0 ~0
-- step keyless: change rows
DELETE FROM rt.k WHERE a = 5;
INSERT INTO rt.k VALUES (500, 'x', NULL, 1), (500, 'x', NULL, 1);
-- expect
rows k +2 -20 ~0
