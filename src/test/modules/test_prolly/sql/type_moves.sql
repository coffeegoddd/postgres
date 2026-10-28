-- Moving a branch to a commit that defines types or materialized views
-- otherwise (a reset, a fast-forward): each changed type is made again
-- under its name and the columns using it converted, before the rows are
-- written; materialized views are made again or refreshed after.  The
-- branch then folds to the commit's root.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA tm VERSIONED;
CREATE TYPE tm.mood AS ENUM ('sad', 'ok');
CREATE DOMAIN tm.pos AS int CHECK (VALUE > 0);
CREATE TYPE tm.pair AS (x int, y int);
CREATE TABLE tm.t (id int PRIMARY KEY, m tm.mood, ms tm.mood[], p tm.pos, pr tm.pair);
INSERT INTO tm.t VALUES (1, 'sad', '{sad,ok}', 1, ROW(1, 2)), (2, 'ok', '{}', 2, ROW(3, 4));
CREATE MATERIALIZED VIEW tm.counts AS SELECT m, count(*) AS n FROM tm.t GROUP BY m;
CREATE MATERIALIZED VIEW tm.ids AS SELECT id FROM tm.t;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');
SELECT dolt_checkout('feature');

-- An enum gains a label and a row uses it; a domain's check changes; a
-- composite gains an attribute; a materialized view's query changes and
-- another's rows.
ALTER TYPE tm.mood ADD VALUE 'happy';
INSERT INTO tm.t VALUES (3, 'happy', '{happy}', 3, ROW(5, 6));
ALTER DOMAIN tm.pos DROP CONSTRAINT pos_check;
ALTER DOMAIN tm.pos ADD CONSTRAINT pos_check CHECK (VALUE > 0 AND VALUE < 100);
ALTER TYPE tm.pair ADD ATTRIBUTE z int;
DROP MATERIALIZED VIEW tm.counts;
CREATE MATERIALIZED VIEW tm.counts AS SELECT m, count(*) AS n, max(id) AS last FROM tm.t GROUP BY m;
REFRESH MATERIALIZED VIEW tm.ids;
SELECT length(dolt_commit('-Am', 'feature types')::text) AS committed;
SELECT dolt_checkout('main');

-- A fast-forward moves main there.
SELECT (dolt_merge('feature'))[2:4];
SELECT test_fold_root() = prolly_root_hash('feature') AS same_root;
SELECT * FROM tm.t ORDER BY id;
SELECT enum_range(NULL::tm.mood);
SELECT * FROM tm.counts ORDER BY m;
SELECT count(*) FROM tm.ids;
SELECT count(*) AS no_leftovers FROM pg_type WHERE typname LIKE 'prolly_moving_%';

-- And back: a label goes (a row using it goes first), the rest as before.
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT test_fold_root() = prolly_root_hash('HEAD') AS same_root;
SELECT * FROM tm.t ORDER BY id;
SELECT enum_range(NULL::tm.mood);
SELECT * FROM tm.counts ORDER BY m;

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA tm CASCADE;

-- What uses a changed type moves with it: a composite type and a domain
-- over it, a routine, a default, a check and a partial index; an array of
-- a composite that gains an attribute of a type new to it; a domain's
-- check that rows about to go would fail; a materialized view over one
-- that changes, committed stale; and back, a NOT NULL column's label going.
CREATE SCHEMA tm2 VERSIONED;
CREATE TYPE tm2.mood AS ENUM ('sad', 'ok');
CREATE TYPE tm2.wrap AS (m tm2.mood);
CREATE DOMAIN tm2.calm AS tm2.mood CHECK (VALUE <> 'sad');
CREATE FUNCTION tm2.f(m tm2.mood) RETURNS text LANGUAGE sql AS $$ SELECT m::text $$;
CREATE DOMAIN tm2.pos AS int CHECK (VALUE > 0);
CREATE TYPE tm2.pair AS (x int, y int);
CREATE TABLE tm2.t (id int PRIMARY KEY, m tm2.mood NOT NULL DEFAULT 'ok',
	w tm2.wrap, c tm2.calm, p tm2.pos, prs tm2.pair[],
	CONSTRAINT not_sad CHECK (m <> 'sad' OR id > 0));
CREATE INDEX t_ok ON tm2.t (id) WHERE m = 'ok';
INSERT INTO tm2.t VALUES (1, 'ok', ROW('ok'), 'ok', 50, '{"(1,2)"}'),
	(2, 'sad', ROW('sad'), NULL, 500, '{}');
CREATE MATERIALIZED VIEW tm2.mv1 AS SELECT id FROM tm2.t;
CREATE MATERIALIZED VIEW tm2.mv2 AS SELECT count(*) AS n FROM tm2.mv1;
SELECT length(dolt_commit('-Am', 'tm2 base')::text) AS committed;
SELECT dolt_branch('feature2');
SELECT dolt_checkout('feature2');
ALTER TYPE tm2.mood ADD VALUE 'happy';
CREATE TYPE tm2.color AS ENUM ('red', 'blue');
ALTER TYPE tm2.pair ADD ATTRIBUTE c tm2.color;
DELETE FROM tm2.t WHERE id = 2;
ALTER DOMAIN tm2.pos DROP CONSTRAINT pos_check;
ALTER DOMAIN tm2.pos ADD CONSTRAINT pos_check CHECK (VALUE > 0 AND VALUE < 100);
INSERT INTO tm2.t VALUES (3, 'happy', ROW('happy'), 'happy', 3, '{"(5,6,red)"}');
DROP MATERIALIZED VIEW tm2.mv2;
DROP MATERIALIZED VIEW tm2.mv1;
CREATE MATERIALIZED VIEW tm2.mv1 AS SELECT id, m FROM tm2.t;
CREATE MATERIALIZED VIEW tm2.mv2 AS SELECT count(*) AS n FROM tm2.mv1;
INSERT INTO tm2.t VALUES (4, 'ok', NULL, NULL, 4, NULL);	-- (the views stale)
SELECT length(dolt_commit('-Am', 'tm2 feature')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature2'))[2:4];
SELECT test_fold_root() = prolly_root_hash('feature2') AS same_root;
SELECT id, m, w, c, p, prs FROM tm2.t ORDER BY id;
SELECT tm2.f('happy'), (SELECT count(*) FROM tm2.mv1) AS mv1_stale, (SELECT n FROM tm2.mv2) AS mv2;
SELECT count(*) AS no_leftovers FROM pg_type WHERE typname LIKE 'prolly_moving_%';
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT test_fold_root() = prolly_root_hash('HEAD') AS same_root;
SELECT id, m, w, c, p, prs FROM tm2.t ORDER BY id;

SELECT dolt_branch('-D', 'feature2');
DROP SCHEMA tm2 CASCADE;

-- Unvalidated domain checks must be added with ALTER DOMAIN, preserving
-- their validation state.  Mix them with validated checks (including one
-- whose expression contains NOT VALID), and cover both changed and new
-- domains, quoted names, dependencies, fast-forward and reset.
CREATE SCHEMA tm3 VERSIONED;
CREATE DOMAIN tm3."nv domain" AS int DEFAULT 1
  CONSTRAINT present NOT NULL
  CONSTRAINT bounded CHECK (VALUE < 100)
  CONSTRAINT words CHECK (VALUE::text <> ' NOT VALID');
CREATE DOMAIN tm3.child AS tm3."nv domain";
CREATE TABLE tm3.t (pk int PRIMARY KEY, v tm3."nv domain");
INSERT INTO tm3.t VALUES (1, 2);
SELECT length(dolt_commit('-Am', 'tm3 base')::text) AS committed;
SELECT dolt_branch('not_valid');
SELECT dolt_checkout('not_valid');
ALTER DOMAIN tm3."nv domain" ADD CONSTRAINT "positive check" CHECK (VALUE > 0) NOT VALID;
ALTER DOMAIN tm3."nv domain" ADD CONSTRAINT even CHECK (VALUE % 2 = 0) NOT VALID;
ALTER DOMAIN tm3.child ADD CONSTRAINT small CHECK (VALUE < 10) NOT VALID;
CREATE DOMAIN tm3.added AS int;
ALTER DOMAIN tm3.added ADD CONSTRAINT positive CHECK (VALUE > 0) NOT VALID;
SELECT length(dolt_commit('-am', 'unvalidated checks')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('not_valid'))[2:4];
SELECT test_fold_root() = prolly_root_hash('not_valid') AS same_root;
SELECT t.typname, c.conname, c.convalidated
FROM pg_constraint c JOIN pg_type t ON t.oid = c.contypid
WHERE t.typnamespace = 'tm3'::regnamespace
ORDER BY t.typname, c.conname;
SELECT * FROM tm3.t;
-- Unvalidated checks still reject new values, as do the inline constraints.
INSERT INTO tm3.t VALUES (2, 3);
INSERT INTO tm3.t VALUES (2, -2);
INSERT INTO tm3.t VALUES (2, 100);
INSERT INTO tm3.t VALUES (2, NULL);
SELECT 12::tm3.child;
-- Restoring the base and then the constrained commit exercises both ways.
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT test_fold_root() = prolly_root_hash('HEAD') AS same_root;
SELECT dolt_reset('--hard', 'not_valid');
SELECT test_fold_root() = prolly_root_hash('not_valid') AS same_root;
SELECT * FROM tm3.t;
SELECT count(*) AS no_leftovers FROM pg_type WHERE typname LIKE 'prolly_moving_%';
-- A stored value an unvalidated check rejects, then the domain changed
-- otherwise: a reset across both keeps the value, the column moving to
-- the changed domain before its unvalidated checks are added.
ALTER DOMAIN tm3."nv domain" DROP CONSTRAINT "positive check";
INSERT INTO tm3.t VALUES (3, -4);
ALTER DOMAIN tm3."nv domain" ADD CONSTRAINT "positive check" CHECK (VALUE > 0) NOT VALID;
SELECT (dolt_commit('-am', 'stored value an unvalidated check rejects'))[1] AS nv_stored \gset
ALTER DOMAIN tm3."nv domain" SET DEFAULT 4;
SELECT (dolt_commit('-am', 'domain default'))[1] AS nv_default \gset
SELECT dolt_reset('--hard', :'nv_stored');
SELECT test_fold_root() = prolly_root_hash(:'nv_stored') AS same_root;
SELECT * FROM tm3.t ORDER BY pk;
SELECT dolt_reset('--hard', :'nv_default');
SELECT test_fold_root() = prolly_root_hash(:'nv_default') AS same_root;
SELECT * FROM tm3.t ORDER BY pk;
SELECT dolt_reset('--hard', 'not_valid');
SELECT dolt_reset('--hard', :'nv_default');
SELECT test_fold_root() = prolly_root_hash(:'nv_default') AS same_root;
SELECT * FROM tm3.t ORDER BY pk;
SELECT c.conname, c.convalidated FROM pg_constraint c
WHERE c.contypid = 'tm3."nv domain"'::regtype ORDER BY c.conname;
SELECT * FROM dolt_status();
SELECT dolt_branch('-D', 'not_valid');
DROP SCHEMA tm3 CASCADE;

-- A default that depends on a redefined type, on a column whose line
-- changed otherwise too (its statistics target, its storage): a move
-- across both puts the default back, as each version has it.
CREATE SCHEMA tm4 VERSIONED;
CREATE DOMAIN tm4.d AS text;
CREATE TYPE tm4.e AS ENUM ('a', 'b');
CREATE TABLE tm4.t (id int PRIMARY KEY, c tm4.d DEFAULT 'x'::tm4.d, m tm4.e DEFAULT 'a');
INSERT INTO tm4.t (id) VALUES (1);
SELECT length(dolt_commit('-Am', 'tm4 one')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm4_one \gset
ALTER DOMAIN tm4.d ADD CONSTRAINT short CHECK (length(VALUE) < 10);
ALTER TABLE tm4.t ALTER COLUMN c SET STATISTICS 100;
ALTER TYPE tm4.e RENAME VALUE 'b' TO 'bee';
ALTER TABLE tm4.t ALTER COLUMN m SET STORAGE EXTERNAL;
SELECT length(dolt_commit('-am', 'tm4 two')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm4_two \gset
SELECT dolt_reset('--hard', :'tm4_one');
SELECT a.attname, pg_get_expr(d.adbin, d.adrelid) FROM pg_attrdef d
JOIN pg_attribute a ON a.attrelid = d.adrelid AND a.attnum = d.adnum
WHERE d.adrelid = 'tm4.t'::regclass ORDER BY 1;
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_reset('--hard', :'tm4_two');
SELECT a.attname, pg_get_expr(d.adbin, d.adrelid) FROM pg_attrdef d
JOIN pg_attribute a ON a.attrelid = d.adrelid AND a.attnum = d.adnum
WHERE d.adrelid = 'tm4.t'::regclass ORDER BY 1;
SELECT count(*) AS changed FROM dolt_status;
INSERT INTO tm4.t (id) VALUES (2);
SELECT * FROM tm4.t ORDER BY id;
DROP SCHEMA tm4 CASCADE;
SELECT length(dolt_commit('-Am', 'tm4 gone')::text) AS committed;

-- A type the commit defines otherwise, used by a composite type and a
-- domain the commit doesn't define at all (with tables using them): those
-- go, keeping the old type until they do, rather than being refused
CREATE SCHEMA tm5 VERSIONED;
CREATE DOMAIN tm5.d AS int;
SELECT length(dolt_commit('-Am', 'tm5 one')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm5_one \gset
ALTER DOMAIN tm5.d ADD CONSTRAINT k CHECK (VALUE > 0);
CREATE TYPE tm5.comp AS (x tm5.d);
CREATE DOMAIN tm5.d2 AS tm5.d;
CREATE TABLE tm5.t (id int PRIMARY KEY, c tm5.comp, c2 tm5.d2);
INSERT INTO tm5.t VALUES (1, ROW(1), 2);
SELECT length(dolt_commit('-Am', 'tm5 two')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm5_two \gset
SELECT dolt_reset('--hard', :'tm5_one') IS NOT NULL AS reset;
SELECT to_regtype('tm5.comp') IS NULL AS comp_gone,
	   to_regtype('tm5.d2') IS NULL AS d2_gone,
	   to_regclass('tm5.t') IS NULL AS t_gone;
SELECT conname FROM pg_constraint WHERE contypid = 'tm5.d'::regtype;
SELECT count(*) AS aside FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace
WHERE n.nspname = 'tm5' AND t.typname LIKE 'prolly_moving%';
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_reset('--hard', :'tm5_two') IS NOT NULL AS reset;
SELECT * FROM tm5.t;
SELECT conname FROM pg_constraint WHERE contypid = 'tm5.d'::regtype;
DROP SCHEMA tm5 CASCADE;
SELECT length(dolt_commit('-Am', 'tm5 gone')::text) AS committed;


-- ... and a domain going over a domain moved with it, kept (over the
-- one changed): every type renamed aside is dropped once that goes
CREATE SCHEMA tm6 VERSIONED;
CREATE DOMAIN tm6.d AS int;
CREATE DOMAIN tm6.d2 AS tm6.d;
CREATE TABLE tm6.k (id int PRIMARY KEY, a tm6.d2);
INSERT INTO tm6.k VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'tm6 one')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm6_one \gset
ALTER TABLE tm6.k DROP COLUMN a;
ALTER DOMAIN tm6.d ADD CONSTRAINT k CHECK (VALUE > 0);
ALTER TABLE tm6.k ADD COLUMN a tm6.d2;
UPDATE tm6.k SET a = 1;
CREATE DOMAIN tm6.d3 AS tm6.d2;
CREATE TABLE tm6.g (id int PRIMARY KEY, d3 tm6.d3);
INSERT INTO tm6.g VALUES (1, 3);
SELECT length(dolt_commit('-Am', 'tm6 two')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm6_two \gset
SELECT dolt_reset('--hard', :'tm6_one') IS NOT NULL AS reset;
SELECT t.typname FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace
WHERE n.nspname = 'tm6' AND t.typtype IN ('d', 'c', 'r')
  AND (t.typrelid = 0 OR t.typrelid IN (SELECT oid FROM pg_class WHERE relkind = 'c'))
ORDER BY 1;
SELECT * FROM tm6.k;
SELECT conname FROM pg_constraint WHERE contypid = 'tm6.d'::regtype;
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_reset('--hard', :'tm6_two') IS NOT NULL AS reset;
SELECT * FROM tm6.g;
DROP SCHEMA tm6 CASCADE;
SELECT length(dolt_commit('-Am', 'tm6 gone')::text) AS committed;

-- ... a composite type going with a column of a composite type moved
-- with it, kept
CREATE SCHEMA tm7 VERSIONED;
CREATE DOMAIN tm7.d AS int;
CREATE TYPE tm7.inner AS (x tm7.d);
CREATE TABLE tm7.k (id int PRIMARY KEY, b tm7.inner);
INSERT INTO tm7.k VALUES (1, ROW(1));
SELECT length(dolt_commit('-Am', 'tm7 one')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm7_one \gset
ALTER TABLE tm7.k DROP COLUMN b;
ALTER DOMAIN tm7.d ADD CONSTRAINT k CHECK (VALUE > 0);
ALTER TABLE tm7.k ADD COLUMN b tm7.inner;
UPDATE tm7.k SET b = ROW(1);
CREATE TYPE tm7.outer AS (i tm7.inner);
CREATE TABLE tm7.g (id int PRIMARY KEY, o tm7.outer);
INSERT INTO tm7.g VALUES (1, ROW(ROW(4)));
SELECT length(dolt_commit('-Am', 'tm7 two')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm7_two \gset
SELECT dolt_reset('--hard', :'tm7_one') IS NOT NULL AS reset;
SELECT t.typname FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace
WHERE n.nspname = 'tm7' AND t.typtype IN ('d', 'c', 'r')
  AND (t.typrelid = 0 OR t.typrelid IN (SELECT oid FROM pg_class WHERE relkind = 'c'))
ORDER BY 1;
SELECT * FROM tm7.k;
SELECT conname FROM pg_constraint WHERE contypid = 'tm7.d'::regtype;
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_reset('--hard', :'tm7_two') IS NOT NULL AS reset;
SELECT * FROM tm7.g;
DROP SCHEMA tm7 CASCADE;
SELECT length(dolt_commit('-Am', 'tm7 gone')::text) AS committed;

-- ... a typed table going, of a composite type going: its columns aren't
-- converted (a typed table's can't be), they go with it
CREATE SCHEMA tm8 VERSIONED;
CREATE DOMAIN tm8.d AS int;
SELECT length(dolt_commit('-Am', 'tm8 one')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm8_one \gset
ALTER DOMAIN tm8.d ADD CONSTRAINT k CHECK (VALUE > 0);
CREATE TYPE tm8.c AS (x tm8.d);
CREATE TABLE tm8.u OF tm8.c;
INSERT INTO tm8.u VALUES (5);
SELECT length(dolt_commit('-Am', 'tm8 two')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm8_two \gset
SELECT dolt_reset('--hard', :'tm8_one') IS NOT NULL AS reset;
SELECT t.typname FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace
WHERE n.nspname = 'tm8' AND t.typtype IN ('d', 'c', 'r')
  AND (t.typrelid = 0 OR t.typrelid IN (SELECT oid FROM pg_class WHERE relkind = 'c'))
ORDER BY 1;
SELECT to_regclass('tm8.u') IS NULL AS u_gone;
SELECT conname FROM pg_constraint WHERE contypid = 'tm8.d'::regtype;
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_reset('--hard', :'tm8_two') IS NOT NULL AS reset;
SELECT * FROM tm8.u;
DROP SCHEMA tm8 CASCADE;
SELECT length(dolt_commit('-Am', 'tm8 gone')::text) AS committed;

-- ... a range type going over the type changed
CREATE SCHEMA tm9 VERSIONED;
CREATE DOMAIN tm9.d AS int;
SELECT length(dolt_commit('-Am', 'tm9 one')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm9_one \gset
ALTER DOMAIN tm9.d ADD CONSTRAINT k CHECK (VALUE > 0);
CREATE TYPE tm9.r AS RANGE (subtype = tm9.d);
CREATE TABLE tm9.g (id int PRIMARY KEY, r tm9.r);
INSERT INTO tm9.g VALUES (1, '[1,5)');
SELECT length(dolt_commit('-Am', 'tm9 two')::text) AS committed;
SELECT dolt_hashof('HEAD') AS tm9_two \gset
SELECT dolt_reset('--hard', :'tm9_one') IS NOT NULL AS reset;
SELECT t.typname FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace
WHERE n.nspname = 'tm9' AND t.typtype IN ('d', 'c', 'r')
  AND (t.typrelid = 0 OR t.typrelid IN (SELECT oid FROM pg_class WHERE relkind = 'c'))
ORDER BY 1;
SELECT conname FROM pg_constraint WHERE contypid = 'tm9.d'::regtype;
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_reset('--hard', :'tm9_two') IS NOT NULL AS reset;
SELECT * FROM tm9.g;
DROP SCHEMA tm9 CASCADE;
SELECT length(dolt_commit('-Am', 'tm9 gone')::text) AS committed;
