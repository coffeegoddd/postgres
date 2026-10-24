--
-- PostGIS's lifecycle with commits in place, in a database of its own:
-- ALTER EXTENSION postgis UPDATE, with committed and uncommitted
-- versioned data on two branches, keeps every value and index, leaves
-- nothing new to commit and still enforces typmods; commits from before
-- and after it check out, reset, diff and merge.  (A PostGIS build has one
-- version, so the update goes to PostGIS's "ANY" version, which runs no
-- SQL, and back from it through the upgrade script every PostGIS release
-- ships, which replaces each of its functions, operators and operator
-- classes.)  DROP EXTENSION postgis is refused while versioned columns on
-- another branch use it, CASCADE or not.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

SELECT current_database() AS db \gset
CREATE DATABASE regress_gis_ext;
\c regress_gis_ext
SET client_min_messages = warning;
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE EXTENSION postgis;
SELECT extversion AS v0 FROM pg_extension WHERE extname = 'postgis' \gset

-- a query's rows' EWKB, and a GiST scan against a sequential one (in an
-- ordinary schema, so they stay through moves)
CREATE SCHEMA gis_ext_check;
CREATE FUNCTION gis_ext_check.digest(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE out text;
BEGIN
  EXECUTE format($f$SELECT count(*) || ' ' || md5(string_agg(id || ':' || encode(ST_AsEWKB(g), 'hex'), ',' ORDER BY id)) FROM (%s) s$f$, q) INTO out;
  RETURN out;
END $$;
CREATE FUNCTION gis_ext_check.agree(tbl text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
  q text := format('SELECT * FROM %I WHERE g && ST_MakeEnvelope(-50, -50, 50, 50, 4326)', tbl);
  plan text := '';
  line text;
  i text;
  s text;
BEGIN
  SET LOCAL enable_seqscan = off;
  SET LOCAL enable_bitmapscan = off;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line; END LOOP;
  i := gis_ext_check.digest(q);
  SET LOCAL enable_seqscan = on;
  SET LOCAL enable_indexscan = off;
  s := gis_ext_check.digest(q);
  RETURN plan LIKE '%Index Scan%' AND i = s;
END $$;
\set c1_rows 'SELECT id, ST_SetSRID(ST_MakePoint(id, -id), 4326) AS g FROM generate_series(1, 200) id'

CREATE TABLE gis_ext (id int PRIMARY KEY, g geometry(Point, 4326));
CREATE INDEX ON gis_ext USING gist (g);
INSERT INTO gis_ext SELECT id, g FROM (:c1_rows) s;
SELECT 'c1', (dolt_commit('-Am', 'gis ext c1'))[1] ~ '^[0-9a-f]{40}$';
SELECT dolt_hashof('HEAD') AS c1 \gset
SELECT 'branch side', (dolt_branch('side'))[1] = '0';
SELECT 'checkout side', (dolt_checkout('side'))[1] = '0';
CREATE TABLE gis_side (id int PRIMARY KEY, g geometry(Polygon, 4326));
CREATE INDEX ON gis_side USING gist (g);
INSERT INTO gis_side SELECT i, ST_Buffer(ST_SetSRID(ST_MakePoint(i, i), 4326), 2, 3) FROM generate_series(1, 40) i;
SELECT 'side commit', (dolt_commit('-Am', 'gis side'))[1] ~ '^[0-9a-f]{40}$';
-- uncommitted on side
UPDATE gis_side SET g = ST_Translate(g, 1, 0) WHERE id <= 5;
SELECT gis_ext_check.digest('SELECT * FROM gis_side') AS side_before \gset
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';

-- with every dependent on main gone, those on side still refuse the drop
DROP TABLE gis_ext;
\set ON_ERROR_STOP 0
\set VERBOSITY terse
DROP EXTENSION postgis;
DROP EXTENSION postgis CASCADE;
\set VERBOSITY default
\set ON_ERROR_STOP 1
SELECT 'still installed', count(*) FROM pg_extension WHERE extname = 'postgis';
SELECT 'reset main', (dolt_reset('--hard', :'c1'))[1] = '0';
SELECT 'checkout side', (dolt_checkout('side'))[1] = '0';
SELECT 'side intact', gis_ext_check.digest('SELECT * FROM gis_side') = :'side_before';
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';
-- uncommitted on main
UPDATE gis_ext SET g = ST_SetSRID(ST_MakePoint(-id, id), 4326) WHERE id <= 20;
SELECT gis_ext_check.digest('SELECT * FROM gis_ext') AS main_before \gset

-- to ANY
ALTER EXTENSION postgis UPDATE TO 'ANY';
SELECT 'version', extversion FROM pg_extension WHERE extname = 'postgis';
SELECT 'main values', gis_ext_check.digest('SELECT * FROM gis_ext') = :'main_before';
SELECT 'main index', gis_ext_check.agree('gis_ext');
SELECT 'main status', string_agg(table_name || ' ' || status, ',' ORDER BY table_name) FROM dolt_status;
SELECT 'commit at ANY', (dolt_commit('-am', 'gis ext at ANY'))[1] ~ '^[0-9a-f]{40}$';
SELECT dolt_hashof('HEAD') AS c2 \gset

-- and back, through the upgrade script
ALTER EXTENSION postgis UPDATE;
SELECT 'version again', extversion = :'v0' FROM pg_extension WHERE extname = 'postgis';
SELECT 'main values', gis_ext_check.digest('SELECT * FROM gis_ext') = :'main_before';
SELECT 'main index', gis_ext_check.agree('gis_ext');
SELECT 'main status', count(*) FROM dolt_status;
SELECT 'checkout side', (dolt_checkout('side'))[1] = '0';
SELECT 'side values', gis_ext_check.digest('SELECT * FROM gis_side') = :'side_before';
SELECT 'side index', gis_ext_check.agree('gis_side');
SELECT 'side status', string_agg(table_name || ' ' || status, ',' ORDER BY table_name) FROM dolt_status;
SELECT 'side commit', (dolt_commit('-am', 'gis side after the update'))[1] ~ '^[0-9a-f]{40}$';
-- the typmod is still enforced
\set ON_ERROR_STOP 0
\set VERBOSITY terse
INSERT INTO gis_side VALUES (100, 'SRID=4326;POINT(0 0)');
\set VERBOSITY default
\set ON_ERROR_STOP 1
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';

-- commits from before and after the update
SELECT 'merge side', (dolt_merge('side'))[2:4];
SELECT 'merged', gis_ext_check.digest('SELECT * FROM gis_side') = :'side_before';
SELECT 'merged index', gis_ext_check.agree('gis_side');
SELECT 'diff c1..c2', count(*), string_agg(DISTINCT diff_type, ',')
FROM dolt_diff(:'c1', :'c2', NULL::gis_ext);
SELECT 'reset to c1', (dolt_reset('--hard', :'c1'))[1] = '0';
SELECT 'c1 values', gis_ext_check.digest('SELECT * FROM gis_ext') = gis_ext_check.digest(:'c1_rows');
SELECT 'c1 index', gis_ext_check.agree('gis_ext');
SELECT 'reset to c2', (dolt_reset('--hard', :'c2'))[1] = '0';
SELECT 'c2 values', gis_ext_check.digest('SELECT * FROM gis_ext') = :'main_before';
SELECT 'branch at c1', (dolt_branch('old', :'c1'))[1] = '0';
SELECT 'checkout old', (dolt_checkout('old'))[1] = '0';
SELECT 'old values', gis_ext_check.digest('SELECT * FROM gis_ext') = gis_ext_check.digest(:'c1_rows');
SELECT 'old index', gis_ext_check.agree('gis_ext');
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';

\c :db
DROP DATABASE regress_gis_ext;
