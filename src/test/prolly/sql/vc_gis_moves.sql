--
-- PostGIS values and indexes through every move: each table's EWKB
-- (raster WKB) is byte-identical after checkout, reset back and forward,
-- a merge into a branch that never had the tables, cherry-pick, revert
-- and revert of the revert; the spatial indexes the moves maintain agree
-- with sequential scans; typmods are kept and enforced.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

\getenv abs_srcdir PG_ABS_SRCDIR
\set vc_setup :abs_srcdir '/sql/vc_versioned_public.sql'
\i :vc_setup

CREATE TABLE gis_anchor (id int PRIMARY KEY);
INSERT INTO gis_anchor VALUES (1);
SELECT 'anchor', (dolt_commit('-Am', 'gis anchor'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'branch bare', (dolt_branch('gis_bare'))[1] = '0';

\set gis_corpus :abs_srcdir '/sql/gis_corpus.sql'
\i :gis_corpus

-- indexes the moves maintain, one per table so each is the one a plan
-- can use
CREATE TABLE gis_m_gist AS SELECT * FROM gis_idx;
ALTER TABLE gis_m_gist ADD PRIMARY KEY (id);
CREATE INDEX ON gis_m_gist USING gist (g);
CREATE TABLE gis_m_nd AS SELECT * FROM gis_idx;
ALTER TABLE gis_m_nd ADD PRIMARY KEY (id);
CREATE INDEX ON gis_m_nd USING gist (g gist_geometry_ops_nd);
CREATE TABLE gis_m_spgist AS SELECT * FROM gis_idx;
ALTER TABLE gis_m_spgist ADD PRIMARY KEY (id);
CREATE INDEX ON gis_m_spgist USING spgist (g spgist_geometry_ops_3d);
CREATE TABLE gis_m_brin AS SELECT * FROM gis_idx;
ALTER TABLE gis_m_brin ADD PRIMARY KEY (id);
CREATE INDEX ON gis_m_brin USING brin (g) WITH (pages_per_range = 1);
CREATE TABLE gis_m_geog AS SELECT * FROM gis_geogs;
ALTER TABLE gis_m_geog ADD PRIMARY KEY (id);
CREATE INDEX ON gis_m_geog USING gist (g);
CREATE TABLE gis_typed (id int PRIMARY KEY, p geometry(PointZ, 4326), g geography(Polygon, 4326));
INSERT INTO gis_typed VALUES (1, 'SRID=4326;POINT Z (1 2 3)', 'POLYGON((0 0, 1 0, 1 1, 0 0))');

-- The state: every table's digest, and whether each maintained index
-- agrees with a sequential scan.
CREATE FUNCTION gis_check.state() RETURNS text LANGUAGE sql AS $$
  SELECT md5(string_agg(t || '=' || gis_check.digest(t::regclass), ',' ORDER BY t))
  FROM unnest(ARRAY['gis_geoms', 'gis_geogs', 'gis_idx', 'gis_rasters', 'gis_m_gist', 'gis_m_nd',
                    'gis_m_spgist', 'gis_m_brin', 'gis_m_geog', 'gis_typed']) t
$$;
CREATE FUNCTION gis_check.indexes() RETURNS text LANGUAGE sql AS $$
  SELECT string_agg(substr(t, 7) || ' ' || r.result, ', ' ORDER BY t) FROM unnest(ARRAY['gis_m_gist', 'gis_m_nd',
    'gis_m_spgist', 'gis_m_brin', 'gis_m_geog']) t,
  LATERAL (SELECT gis_check.scans_agree(t::regclass, i.indexrelid::regclass) AS result
           FROM pg_index i WHERE i.indrelid = t::regclass AND NOT i.indisprimary) r
$$;
CREATE FUNCTION gis_check.typmods() RETURNS text LANGUAGE sql AS $$
  SELECT string_agg(format_type(atttypid, atttypmod), ',' ORDER BY attnum) FROM pg_attribute
  WHERE attrelid = 'gis_typed'::regclass AND attnum > 1
$$;

SELECT 'tables on prolly', bool_and(amname = 'prolly') FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE relname LIKE 'gis\_%' AND relkind = 'r';
SELECT 'indexes before', gis_check.indexes();
SELECT gis_check.state() AS s1 \gset
SELECT (dolt_commit('-Am', 'gis corpus'))[1] AS c1 \gset

-- a new branch's first use builds its tables from the commit
SELECT 'branch', (dolt_branch('gis_b1'))[1] = '0';
SELECT 'checkout', (dolt_checkout('gis_b1'))[1] = '0';
SELECT 'checkout: same', gis_check.state() = :'s1';
SELECT 'checkout: indexes', gis_check.indexes();
SELECT 'checkout: typmods', gis_check.typmods();
SELECT 'checkout: no Z refused', gis_check.outcome($$INSERT INTO gis_typed VALUES (2, 'SRID=4326;POINT(1 2)', NULL)$$);
SELECT 'checkout: a point refused', gis_check.outcome($$INSERT INTO gis_typed VALUES (3, NULL, 'SRID=4326;POINT(1 2)')$$);
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';

-- edits of every kind, committed
UPDATE gis_geoms SET g = ST_Translate(g, 1, 1) WHERE id IN (1, 9, 16, 38);
UPDATE gis_geoms SET g = ST_Buffer('POINT(5 5)'::geometry, 900, 18000) WHERE id = 39;
DELETE FROM gis_geoms WHERE id IN (2, 22, 31);
INSERT INTO gis_geoms VALUES (40, 'POINT(7 7)'), (41, 'POLYGON EMPTY');
UPDATE gis_rasters SET r = ST_SetValue(r, 1, 2, 2, 99) WHERE id IN (1, 6);
UPDATE gis_m_gist SET g = ST_Translate(g, 2, 0) WHERE id % 7 = 0;
DELETE FROM gis_m_spgist WHERE id % 9 = 0;
UPDATE gis_m_brin SET g = ST_Translate(g, 50, 50) WHERE id % 11 = 0;
UPDATE gis_m_geog SET g = ST_Project(g, 1000, 0)::geography WHERE id % 5 = 0 AND ST_GeometryType(g::geometry) = 'ST_Point';
INSERT INTO gis_m_nd SELECT id + 1000, ST_Force4D(g) FROM gis_idx WHERE id < 50;
SELECT gis_check.state() AS s2 \gset
SELECT 'edits change the state', :'s2' <> :'s1';
SELECT (dolt_commit('-Am', 'gis edits'))[1] AS c2 \gset

-- reset back to the first commit, and forward again
SELECT 'reset back', (dolt_reset('--hard', :'c1'))[1] = '0';
SELECT 'reset back: same', gis_check.state() = :'s1';
SELECT 'reset back: indexes', gis_check.indexes();
SELECT 'reset forward', (dolt_reset('--hard', :'c2'))[1] = '0';
SELECT 'reset forward: same', gis_check.state() = :'s2';
SELECT 'reset forward: indexes', gis_check.indexes();

-- a merge into a branch that never had the tables writes every row
SELECT 'checkout bare', (dolt_checkout('gis_bare'))[1] = '0';
SELECT 'bare has none', to_regclass('gis_geoms') IS NULL;
INSERT INTO gis_anchor VALUES (2);
SELECT 'commit bare', (dolt_commit('-Am', 'bare'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'merge', (dolt_merge('main'))[2:4];
SELECT 'merge: a merge commit', dolt_hashof('HEAD^2') = dolt_hashof('main');
SELECT 'merge: same', gis_check.state() = :'s2';
SELECT 'merge: indexes', gis_check.indexes();
SELECT 'merge: typmods', gis_check.typmods();

-- cherry-pick of the edits onto a branch at the first commit
SELECT 'branch at c1', (dolt_branch('gis_pick', :'c1'))[1] = '0';
SELECT 'checkout pick', (dolt_checkout('gis_pick'))[1] = '0';
SELECT 'cherry-pick', (dolt_cherry_pick(:'c2'))[2:4];
SELECT 'cherry-pick: same', gis_check.state() = :'s2';
SELECT 'cherry-pick: indexes', gis_check.indexes();

-- revert, and revert of the revert
SELECT 'revert', (dolt_revert('HEAD'))[2:4];
SELECT 'revert: same as c1', gis_check.state() = :'s1';
SELECT 'revert: indexes', gis_check.indexes();
SELECT 'revert again', (dolt_revert('HEAD'))[2:4];
SELECT 'revert again: same as c2', gis_check.state() = :'s2';
SELECT 'revert again: indexes', gis_check.indexes();
SELECT 'typmods', gis_check.typmods();
SELECT 'another SRID refused', gis_check.outcome($$INSERT INTO gis_typed VALUES (4, 'SRID=3857;POINT Z (1 2 3)', NULL)$$);

SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';
