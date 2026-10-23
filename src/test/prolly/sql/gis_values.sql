--
-- PostGIS values in tables of the AM under test read back as written, and
-- PostGIS's functions answer on them as on heap: every geometry type and
-- empty, typmods, large values (out of line, sliced, joined), geography,
-- in-database rasters and spheroids.
--
\getenv abs_srcdir PG_ABS_SRCDIR
\set gis_corpus :abs_srcdir '/sql/gis_corpus.sql'
\set ECHO none
\i :gis_corpus
\set ECHO all

-- the AM under test holds them (extension tables aside)
SELECT relname, amname = current_setting('default_table_access_method') AS on_target
FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE relname LIKE 'gis\_%' AND relkind = 'r' ORDER BY 1;

SELECT 'geoms', gis_check.digest('gis_geoms');
SELECT 'geogs', gis_check.digest('gis_geogs');
SELECT 'idx', gis_check.digest('gis_idx');
SELECT 'rasters', gis_check.digest('gis_rasters');

-- what PostGIS sees in each value
SELECT id, ST_SRID(g), GeometryType(g), ST_NDims(g), ST_HasZ(g), ST_HasM(g),
       ST_IsEmpty(g), ST_NPoints(g), ST_MemSize(g) > 2000 AS large
FROM gis_geoms ORDER BY id;
SELECT id, round(ST_Area(g)::numeric, 3) AS area, round(ST_Length(g)::numeric, 3) AS len
FROM gis_geogs WHERE id < 11 ORDER BY id;
SELECT id, ST_Width(r), ST_Height(r), ST_NumBands(r), ST_BandPixelType(r, 1),
       ST_Value(r, 1, 1, 1), (ST_SummaryStats(r)).sum
FROM gis_rasters ORDER BY id;

-- large values: the bounding box read from a slice of the value's
-- header, the whole value detoasted, and a nested loop over large
-- polygons (PostGIS caches their detoasted forms between rows)
SELECT id, ST_XMin(g), ST_YMax(g), ST_NPoints(g), md5(ST_AsBinary(ST_Simplify(g, 1)))
FROM gis_geoms WHERE id >= 37 ORDER BY id;
CREATE TABLE gis_big AS
SELECT i AS id, ST_Buffer(ST_MakePoint(i * 300, 0), 400, 5000) AS g FROM generate_series(1, 4) i;
CREATE TABLE gis_pts AS
SELECT i AS id, ST_MakePoint(i * 3 % 1700, i * 7 % 900 - 450) AS g FROM generate_series(1, 400) i;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT b.id, count(p.id) FROM gis_big b JOIN gis_pts p ON ST_Contains(b.g, p.g)
GROUP BY b.id ORDER BY b.id;
RESET enable_hashjoin;
RESET enable_mergejoin;

-- typmods are enforced
CREATE TABLE gis_typed (id int PRIMARY KEY, p geometry(PointZ, 4326), g geography(Polygon));
INSERT INTO gis_typed VALUES (1, 'SRID=4326;POINT Z (1 2 3)', 'POLYGON((0 0, 1 0, 1 1, 0 0))');
INSERT INTO gis_typed VALUES (2, 'SRID=4326;POINT(1 2)', NULL);
INSERT INTO gis_typed VALUES (3, 'SRID=3857;POINT Z (1 2 3)', NULL);
INSERT INTO gis_typed VALUES (4, NULL, 'POINT(0 0)');
SELECT format_type(atttypid, atttypmod) FROM pg_attribute
WHERE attrelid = 'gis_typed'::regclass AND attnum > 1 ORDER BY attnum;

-- spheroids (kept as text on a versioned table)
CREATE TABLE gis_spheroids (id int PRIMARY KEY, s spheroid);
INSERT INTO gis_spheroids VALUES (1, 'SPHEROID["WGS 84",6378137,298.257223563]'),
  (2, 'SPHEROID["GRS_1980",6378137,298.257222101]');
SELECT id, s, round(ST_DistanceSpheroid('POINT(-118 34)', 'POINT(-74 40)', s)::numeric, 3)
FROM gis_spheroids ORDER BY id;
