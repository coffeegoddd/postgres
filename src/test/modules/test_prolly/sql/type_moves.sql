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
