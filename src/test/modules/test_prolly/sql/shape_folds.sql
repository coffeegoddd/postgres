-- DDL that changes a table's row shape but no row's bytes (a column added
-- without a default, an enum's label added) keeps its baseline; DDL that
-- changes rows' bytes without writing them (a column added with a
-- default, one dropped, a type relabeled, the key changed) maps the
-- baseline's rows to the shape now.  Either way the next commit reads no
-- table in full, and makes the tree a full fold would
-- (debug_prolly_force_full_fold).
CREATE DATABASE regress_shape_folds;
\c regress_shape_folds
-- (no checkpoint as a rewrite ends, prolly.fold_on_rewrite: what is
-- counted here is what the next fold reads after one)
SET prolly.fold_on_rewrite = off;
-- (every mappable baseline mapped, whatever reading the table would cost:
-- the cost's choice is tested at the end)
SET prolly.map_reshaped_dead_ratio = 0;
CREATE EXTENSION test_prolly;
CREATE FUNCTION full_folds() RETURNS text LANGUAGE sql AS $$
	SELECT coalesce(string_agg(substr(name, 11), ',' ORDER BY name), 'none')
	FROM pg_stat_prolly
	WHERE name LIKE 'fold\_full\_%' AND name <> 'fold_full_tables' AND value > 0
$$;
CREATE FUNCTION same_as_full() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	inc text;
	full_root text;
BEGIN
	inc := test_fold_root();
	SET LOCAL debug_prolly_force_full_fold = on;
	full_root := test_fold_root();
	SET LOCAL debug_prolly_force_full_fold = off;
	RETURN inc = full_root;
END $$;
-- (the incremental root is the full one's; committed, the reasons any
-- table was read in full, and how many baselines were mapped)
\set check 'SELECT same_as_full() AS same; SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly''); SELECT length(dolt_commit(''-Am'', ''ddl'')::text) > 0 AS committed; SELECT pg_stat_force_next_flush(); SELECT full_folds() AS read_in_full, (SELECT value FROM pg_stat_prolly WHERE name = ''fold_mapped_tables'') AS mapped; SELECT count(*) AS verify_mismatches FROM dolt_verify_folds() WHERE NOT matches;'
CREATE SCHEMA sf VERSIONED;
CREATE TYPE sf.mood AS ENUM ('sad', 'ok', 'glad');
CREATE DOMAIN sf.small AS int;
CREATE TABLE sf.t (id int PRIMARY KEY, a int, b text);
INSERT INTO sf.t SELECT g, g % 10, 'b' || g FROM generate_series(1, 2000) g;
-- (an enum keys this one: its labels' order is its rows')
CREATE TABLE sf.e (m sf.mood PRIMARY KEY, n int);
INSERT INTO sf.e VALUES ('sad', 1), ('ok', 2), ('glad', 3);
CREATE TABLE sf.k (a int, b text);
INSERT INTO sf.k SELECT g % 50, 'k' || (g % 7) FROM generate_series(1, 1000) g;
CREATE TABLE sf.long (id int PRIMARY KEY, v text);
ALTER TABLE sf.long ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO sf.long SELECT g, repeat(md5(g::text), 400) FROM generate_series(1, 20) g;
CREATE TABLE sf.nested (id int PRIMARY KEY, ms sf.mood[]);
INSERT INTO sf.nested VALUES (1, '{sad,ok}'), (2, '{glad}');
SELECT length(dolt_commit('-Am', 'shapes')::text) > 0 AS committed;

-- a nullable column added: no row's bytes change (trailing NULLs aren't
-- stored), whatever is written after it
ALTER TABLE sf.t ADD COLUMN c int;
UPDATE sf.t SET c = 1 WHERE id = 1;
:check
-- (a baseline keeps its shape out of line, in a tree of its own, which a
-- collection keeps: the next change of shape reads it)
SELECT dolt_gc();
ALTER TABLE sf.t ADD COLUMN d sf.small;
INSERT INTO sf.t (id, a, b, d) VALUES (5001, 1, 'new', 7);
:check
-- (a domain's constraint: its columns' types are as they were)
ALTER DOMAIN sf.small ADD CONSTRAINT small_check CHECK (VALUE < 1000);
UPDATE sf.t SET d = 8 WHERE id = 5001;
:check
-- (a domain with a constraint added as a column: Postgres rewrites the
-- table to check it, and an ALTER TABLE's rewrite is read in full)
ALTER TABLE sf.t ADD COLUMN dd sf.small;
:check

-- the same, the rows written in its transaction
BEGIN;
ALTER TABLE sf.t ADD COLUMN f text;
UPDATE sf.t SET f = 'f' WHERE id <= 3;
DELETE FROM sf.t WHERE id = 4;
COMMIT;
:check

-- rolled back: the shape the baseline was folded under
BEGIN;
ALTER TABLE sf.t ADD COLUMN g int;
UPDATE sf.t SET g = 1 WHERE id = 5;
ROLLBACK;
UPDATE sf.t SET b = 'changed' WHERE id = 6;
:check

-- keyless: its rows' keys are their bytes
ALTER TABLE sf.k ADD COLUMN c int;
INSERT INTO sf.k VALUES (1, 'k1', 3);
:check

-- values stored out of line
ALTER TABLE sf.long ADD COLUMN w text;
UPDATE sf.long SET w = repeat('w', 5000) WHERE id = 1;
:check

-- an enum's label added, before, after or at the end: the old labels keep
-- their names and their order, so the rows keyed by them theirs
ALTER TYPE sf.mood ADD VALUE 'meh' BEFORE 'ok';
:check
ALTER TYPE sf.mood ADD VALUE 'joy' AFTER 'glad';
ALTER TYPE sf.mood ADD VALUE 'blue' BEFORE 'sad';
INSERT INTO sf.e VALUES ('meh', 4), ('joy', 5), ('blue', 6);
UPDATE sf.nested SET ms = '{blue,meh,joy}' WHERE id = 2;
:check
SELECT m, n FROM sf.e ORDER BY m;
-- (and inside a composite, a composite key, a range and a multirange:
-- each holds the enum's labels as a column of it does)
CREATE TYPE sf.tone AS ENUM ('low', 'high');
CREATE TYPE sf.pair AS (m sf.tone, n int);
CREATE TYPE sf.tones AS RANGE (subtype = sf.tone);
CREATE TABLE sf.comp (id int PRIMARY KEY, p sf.pair);
INSERT INTO sf.comp VALUES (1, ('low', 1)), (2, ('high', 2));
CREATE TABLE sf.compk (p sf.pair PRIMARY KEY, v int);
INSERT INTO sf.compk VALUES (('low', 1), 1), (('high', 2), 2);
CREATE TABLE sf.ranges (id int PRIMARY KEY, r sf.tones, mr sf.tones_multirange);
INSERT INTO sf.ranges VALUES (1, '[low,high]', '{[low,high)}');
SELECT length(dolt_commit('-Am', 'tones')::text) > 0 AS committed;
ALTER TYPE sf.tone ADD VALUE 'mid' BEFORE 'high';
INSERT INTO sf.comp VALUES (3, ('mid', 3));
INSERT INTO sf.compk VALUES (('mid', 3), 3);
INSERT INTO sf.ranges VALUES (2, '[low,mid]', '{[mid,high]}');
:check
SELECT p, v FROM sf.compk ORDER BY p;
-- (a composite's attribute dropped: its values hold it no longer)
ALTER TYPE sf.pair DROP ATTRIBUTE n;
:check
SELECT id, p FROM sf.comp ORDER BY id;

-- a label renamed: rows hold its name, so it changes theirs, and the
-- baseline's can't be read by the type now
ALTER TYPE sf.mood RENAME VALUE 'glad' TO 'happy';
:check

-- DDL that changes rows' bytes without writing them is mapped: the
-- baseline's rows read as they were folded and encoded as the table's are
-- now, not read from the table.  A column added with a default is in every
-- row's bytes.
ALTER TABLE sf.t ADD COLUMN h int DEFAULT 5;
:check
-- (with rows written in its transaction, and a default out of line)
BEGIN;
ALTER TABLE sf.t ADD COLUMN big text DEFAULT repeat('x', 3000);
UPDATE sf.t SET h = 6, big = 'short' WHERE id <= 10;
INSERT INTO sf.t (id, a, b) VALUES (6001, 2, 'later');
DELETE FROM sf.t WHERE id BETWEEN 11 AND 20;
COMMIT;
:check
-- (an enum's, by a table an enum keys)
ALTER TABLE sf.e ADD COLUMN mood sf.mood DEFAULT 'ok';
:check

-- dropped: gone from every row's bytes
ALTER TABLE sf.t DROP COLUMN h;
:check
-- (and one added in its place in the same transaction, with a default)
BEGIN;
ALTER TABLE sf.t DROP COLUMN c;
ALTER TABLE sf.t ADD COLUMN c int DEFAULT 1;
UPDATE sf.t SET c = 2 WHERE id = 21;
COMMIT;
:check

-- rolled back: the shape the baseline was folded under
BEGIN;
ALTER TABLE sf.t ADD COLUMN r int DEFAULT 3;
ALTER TABLE sf.t DROP COLUMN b;
ROLLBACK;
UPDATE sf.t SET b = 'again' WHERE id = 30;
:check

-- keyless: a row's key is its bytes, so mapped rows are sorted again, and
-- rows that differed only in a column dropped become one row, counted
SELECT count(DISTINCT (a, b, c)) AS rows_before, count(DISTINCT a) AS rows_after FROM sf.k;
ALTER TABLE sf.k DROP COLUMN b;
:check
ALTER TABLE sf.k ADD COLUMN m int DEFAULT 9;
DELETE FROM sf.k WHERE a = 1;
:check

-- values stored out of line: their trees are the rows', unread
ALTER TABLE sf.long ADD COLUMN x int DEFAULT 1;
:check
ALTER TABLE sf.long DROP COLUMN w;
:check
ALTER TABLE sf.long DROP COLUMN v;
:check

-- a rewrite that changes no row clears a default's missing value: the
-- rows already hold it
VACUUM FULL sf.k;
INSERT INTO sf.k VALUES (2, 3);
:check

-- a volatile default rewrites the table, which its log says to read in
-- full: not mapped
ALTER TABLE sf.t ADD COLUMN rnd float8 DEFAULT random();
:check

-- changes past maintenance_work_mem are sorted, spilling, and folded from
-- the mapped baseline all the same
CREATE TABLE sf.kb (a int, b text);
INSERT INTO sf.kb SELECT g, 'kb' || g FROM generate_series(1, 3000) g;
SELECT length(dolt_commit('-Am', 'kb')::text) > 0 AS committed;
BEGIN;
ALTER TABLE sf.kb ADD COLUMN m int DEFAULT 9;
UPDATE sf.kb SET a = a + 1;
COMMIT;
SET maintenance_work_mem = '64kB';
:check
RESET maintenance_work_mem;

-- a fold that gives up on the baseline (here, a keyless table emptied,
-- whose counts can't be told apart) reads the table in full and maps
-- nothing: the baseline is mapped only once the finder folds from it
BEGIN;
ALTER TABLE sf.kb ADD COLUMN n int DEFAULT 4;
DELETE FROM sf.kb WHERE a > 10;
TRUNCATE sf.kb;
INSERT INTO sf.kb VALUES (1, 'again');
COMMIT;
:check

-- the change finder "pages" folds from a mapped baseline too
SET debug_prolly_change_finder = pages;
CREATE TABLE sf.p (id int PRIMARY KEY, a int, b text);
INSERT INTO sf.p SELECT g, g, 'p' || g FROM generate_series(1, 500) g;
SELECT length(dolt_commit('-Am', 'pages')::text) > 0 AS committed;
UPDATE sf.p SET a = 0 WHERE id = 1;
SELECT length(dolt_commit('-am', 'pages again')::text) > 0 AS committed;
ALTER TABLE sf.p ADD COLUMN d int DEFAULT 4;
ALTER TABLE sf.p DROP COLUMN a;
UPDATE sf.p SET d = 5 WHERE id = 2;
:check
-- (and a rewrite since, which leaves it no pages to read changes from,
-- reads in full, mapping nothing)
ALTER TABLE sf.p ADD COLUMN e int DEFAULT 6;
VACUUM FULL sf.p;
:check
RESET debug_prolly_change_finder;

-- A column retyped where the type's cast keeps every value as it is
-- (Postgres rewrites no row: a length or precision widened, a
-- binary-coercible type, a domain over it): mapped, each value encoded
-- as the type now's.  A cast that computes rewrites the table, and its log
-- says to read it in full: not mapped (the fold can't know what it did).
CREATE TABLE sf.r (id int PRIMARY KEY, s varchar(10), n numeric(8,2), t text, i int);
INSERT INTO sf.r SELECT g, 's' || g, g / 7.0, CASE WHEN g % 50 = 0 THEN repeat(md5(g::text), 100) ELSE 't' || g END, g FROM generate_series(1, 1000) g;
CREATE TABLE sf.rk (code varchar(10) PRIMARY KEY, v int);
INSERT INTO sf.rk SELECT 'c' || g, 1000 - g FROM generate_series(1, 500) g;
SELECT length(dolt_commit('-Am', 'retyped')::text) > 0 AS committed;
ALTER TABLE sf.r ALTER COLUMN s TYPE varchar(20);
:check
ALTER TABLE sf.r ALTER COLUMN n TYPE numeric(12,2);
UPDATE sf.r SET n = 12345.67 WHERE id = 3;
:check
-- (values out of line too)
ALTER TABLE sf.r ALTER COLUMN t TYPE varchar;
:check
CREATE DOMAIN sf.anyint AS int;
ALTER TABLE sf.r ALTER COLUMN i TYPE sf.anyint;
:check
-- (a key's column: its keys' order is kept, so they stream in order)
ALTER TABLE sf.rk ALTER COLUMN code TYPE varchar(20);
:check
BEGIN;
ALTER TABLE sf.rk ALTER COLUMN code TYPE text;
INSERT INTO sf.rk VALUES ('a much longer code than before', 7);
UPDATE sf.rk SET v = 0 WHERE code = 'c1';
COMMIT;
:check
-- (a cast that computes, and a USING expression whose cast would keep
-- the values: rewrites)
ALTER TABLE sf.r ALTER COLUMN i TYPE bigint;
:check
ALTER TABLE sf.r ALTER COLUMN n TYPE numeric(14,2) USING n * 2;
:check

-- The key changed: the rows mapped are sorted again by the key now, in
-- maintenance_work_mem, spilling past it, as a full fold's are.
CREATE TABLE sf.k2 (a int, b text);
INSERT INTO sf.k2 SELECT 1000 - g, 'k' || g FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-Am', 'keyless')::text) > 0 AS committed;
ALTER TABLE sf.k2 ADD PRIMARY KEY (a);
:check
ALTER TABLE sf.k2 DROP CONSTRAINT k2_pkey;
INSERT INTO sf.k2 VALUES (1, 'k1'), (1, 'k1');
:check
BEGIN;
ALTER TABLE sf.rk DROP CONSTRAINT rk_pkey;
ALTER TABLE sf.rk ADD PRIMARY KEY (v);
UPDATE sf.rk SET code = 'moved' WHERE v = 999;
COMMIT;
:check
SET maintenance_work_mem = '64kB';
ALTER TABLE sf.rk DROP CONSTRAINT rk_pkey;
ALTER TABLE sf.rk ADD PRIMARY KEY (code);
:check
RESET maintenance_work_mem;
-- (rolled back)
BEGIN;
ALTER TABLE sf.rk DROP CONSTRAINT rk_pkey;
ROLLBACK;
UPDATE sf.rk SET v = 1 WHERE code = 'c2';
:check
-- (a row the baseline holds twice can't be a keyed table's, though the
-- table no longer has both: read in full)
DELETE FROM sf.k2 WHERE ctid = (SELECT min(ctid) FROM sf.k2 WHERE a = 1 AND b = 'k1');
DELETE FROM sf.k2 WHERE a = 1 AND b <> 'k1';
ALTER TABLE sf.k2 ADD PRIMARY KEY (a);
:check
-- (nor two rows the baseline holds that share the key now, one deleted
-- since: mapped, the tree would hold the key twice, so read in full)
CREATE TABLE sf.k3 (a int, b text);
INSERT INTO sf.k3 VALUES (1, 'a'), (1, 'b'), (2, 'c');
CREATE TABLE sf.rk2 (a int, b text, PRIMARY KEY (a, b));
INSERT INTO sf.rk2 VALUES (1, 'x'), (1, 'y'), (2, 'z');
SELECT length(dolt_commit('-Am', 'shared keys')::text) > 0 AS committed;
DELETE FROM sf.k3 WHERE b = 'b';
ALTER TABLE sf.k3 ADD PRIMARY KEY (a);
:check
DELETE FROM sf.rk2 WHERE b = 'y';
ALTER TABLE sf.rk2 DROP CONSTRAINT rk2_pkey;
ALTER TABLE sf.rk2 ADD PRIMARY KEY (a);
:check
SELECT * FROM sf.k3 ORDER BY a;
SELECT * FROM sf.rk2 ORDER BY a;

-- mapped only where reading the table costs more: a heap of live rows is
-- read in full, one holding more dead versions than
-- prolly.map_reshaped_dead_ratio a live row is mapped
RESET prolly.map_reshaped_dead_ratio;
CREATE TABLE sf.cost (id int PRIMARY KEY, v int);
INSERT INTO sf.cost SELECT g, 0 FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-Am', 'cost')::text) > 0 AS committed;
SELECT pg_stat_force_next_flush();
ALTER TABLE sf.cost ADD COLUMN w int DEFAULT 7;
SELECT table_name, reason FROM dolt_pending_full_folds() WHERE table_name = 'sf.cost';
:check
UPDATE sf.cost SET v = v + 1;
UPDATE sf.cost SET v = v + 1;
SELECT length(dolt_commit('-am', 'dead versions')::text) > 0 AS committed;
SELECT pg_stat_force_next_flush();
SELECT n_dead_tup >= 1.5 * n_live_tup AS mostly_dead
  FROM pg_stat_user_tables WHERE relid = 'sf.cost'::regclass;
ALTER TABLE sf.cost ADD COLUMN x int DEFAULT 8;
SELECT table_name, reason FROM dolt_pending_full_folds() WHERE table_name = 'sf.cost';
:check
SELECT count(*), sum(w), sum(x) FROM sf.cost;


\c contrib_regression
DROP DATABASE regress_shape_folds;
