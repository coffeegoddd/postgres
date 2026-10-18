-- Range types in versioned schemas: made per branch (their pg_range row,
-- multirange type, constructors and cast with them), held by commits,
-- made and dropped by moves.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA rv VERSIONED;
CREATE TABLE rv.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('rv_other');

CREATE TYPE rv.span AS RANGE (subtype = int4);
CREATE TYPE rv.tspan AS RANGE (subtype = text, collation = "C",
  multirange_type_name = rv.tspans);
CREATE TYPE rv.fspan AS RANGE (subtype = float8,
  subtype_diff = pg_catalog.float8mi);
CREATE TABLE rv.t (id int PRIMARY KEY, s rv.span, ms rv.span_multirange,
  ts rv.tspan, fs rv.fspan);
INSERT INTO rv.t VALUES (1, '[1,5)', '{[1,2),[3,4)}', '[a,c)', '[1.5,2.5]');
SELECT s, s::rv.span_multirange AS cast, rv.span(2, 7) AS made, ts, fs FROM rv.t;
SELECT * FROM dolt_status ORDER BY table_name;
SELECT length(dolt_commit('-Am', 'ranges')::text) AS committed;
SELECT * FROM dolt_status;

-- per branch: another branch has none of it
SELECT dolt_checkout('rv_other');
SELECT count(*) AS ranges FROM pg_range WHERE rngtypid::regtype::text LIKE 'rv.%';
SELECT count(*) AS casts FROM pg_cast WHERE castsource::regtype::text LIKE 'rv.%';
SELECT count(*) AS types FROM pg_type WHERE typnamespace = 'rv'::regnamespace
  AND typname LIKE '%span%';

-- a reset makes them from the commit, and back drops them
SELECT dolt_reset('--hard', 'main');
SELECT * FROM dolt_status;
SELECT s, ms, s::rv.span_multirange AS cast, rv.span(2, 7) AS made, ts, fs,
  pg_typeof(fs - fs) FROM rv.t;
SELECT rngtypid::regtype, rngsubtype::regtype, rngmultitypid::regtype,
  rngcollation::regcollation, rngsubdiff
  FROM pg_range WHERE rngtypid::regtype::text LIKE 'rv.%' ORDER BY 1::text;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT * FROM dolt_status;
SELECT count(*) AS types FROM pg_type WHERE typnamespace = 'rv'::regnamespace
  AND typname LIKE '%span%';
SELECT count(*) AS functions FROM pg_proc WHERE pronamespace = 'rv'::regnamespace;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'rv_other');

-- one changed can't be moved to: Postgres can't change one in place
SELECT dolt_branch('rv_changed');
SELECT dolt_checkout('rv_changed');
DROP TABLE rv.t;
DROP TYPE rv.span;
CREATE TYPE rv.span AS RANGE (subtype = int8);
SELECT length(dolt_commit('-Am', 'span of int8')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('rv_changed') IS NOT NULL AS merged;
SELECT dolt_reset('--hard', 'rv_changed');
SELECT * FROM dolt_status;
SELECT dolt_branch('-D', 'rv_changed');

-- one nothing uses, changed, goes and comes again: an uncommitted
-- redefinition is discarded by a reset
CREATE TYPE rv.lone AS RANGE (subtype = int4);
SELECT length(dolt_commit('-Am', 'lone')::text) AS committed;
DROP TYPE rv.lone;
CREATE TYPE rv.lone AS RANGE (subtype = int8);
SELECT dolt_reset('--hard');
SELECT rngsubtype::regtype FROM pg_range WHERE rngtypid = 'rv.lone'::regtype;
SELECT * FROM dolt_status;

-- one over a domain the move changes: refused, and clearly
CREATE DOMAIN rv.small AS int4 CHECK (VALUE < 100);
CREATE TYPE rv.sspan AS RANGE (subtype = rv.small);
SELECT length(dolt_commit('-Am', 'over a domain')::text) AS committed;
SELECT dolt_branch('rv_domain');
SELECT dolt_checkout('rv_domain');
DROP TYPE rv.sspan;
DROP DOMAIN rv.small;
CREATE DOMAIN rv.small AS int4 CHECK (VALUE < 1000);
CREATE TYPE rv.sspan AS RANGE (subtype = rv.small);
SELECT length(dolt_commit('-Am', 'a wider domain')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_reset('--hard', 'rv_domain');
SELECT dolt_branch('-D', 'rv_domain');
DROP TYPE rv.sspan;
DROP DOMAIN rv.small;
SELECT length(dolt_commit('-Am', 'no domain')::text) AS committed;

-- a multirange outside the range's versioned schema is refused where made
CREATE TYPE rv.pspan AS RANGE (subtype = int4, multirange_type_name = public.pspans);

-- not held: one with a collation of a versioned schema (a move makes
-- types before collations)
CREATE COLLATION rv.coll FROM "C";
CREATE TYPE rv.cspan AS RANGE (subtype = text, collation = rv.coll);
SELECT * FROM dolt_status WHERE status = 'not covered';
SELECT dolt_commit('-Am', 'refused');
DROP TYPE rv.cspan;
DROP COLLATION rv.coll;

-- not held: one with a subtype difference function of a versioned schema
-- (a move makes types before functions)
CREATE FUNCTION rv.diff(int4, int4) RETURNS float8 LANGUAGE sql IMMUTABLE
  AS 'SELECT ($1 - $2)::float8';
CREATE TYPE rv.dspan AS RANGE (subtype = int4, subtype_diff = rv.diff);
SELECT * FROM dolt_status WHERE status = 'not covered';
SELECT dolt_commit('-Am', 'refused');
DROP TYPE rv.dspan;
DROP FUNCTION rv.diff(int4, int4);

DROP SCHEMA rv CASCADE;
SELECT length(dolt_commit('-Am', 'rv gone')::text) AS committed;
