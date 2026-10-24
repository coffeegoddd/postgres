--
-- Out-of-database rasters: a raster band can name a file on the server
-- instead of holding its pixels.  A commit holds the raster's bytes, so
-- the path, band number and metadata come back byte-identical through
-- reset, checkout, merge and a file-remote clone, and the pixels are read
-- from the file wherever the raster is.  The file itself isn't versioned:
-- rewrite it, and every commit's raster reads the new pixels.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

\getenv abs_srcdir PG_ABS_SRCDIR
\set vc_setup :abs_srcdir '/sql/vc_versioned_public.sql'
\i :vc_setup
CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS postgis_raster;
SET postgis.gdal_enabled_drivers = 'GTiff';
SET postgis.enable_outdb_rasters = true;

-- a 4x3 GeoTIFF of 1..12 (then of 101..112), written where the server
-- can read it
SELECT current_setting('data_directory') || '/gis_outdb_' || txid_current() || '.tif' AS path \gset
CREATE SCHEMA gis_outdb_check;
CREATE FUNCTION gis_outdb_check.write_tif(path text, base int) RETURNS int LANGUAGE plpgsql AS $$
DECLARE lo oid;
BEGIN
  lo := lo_from_bytea(0, ST_AsGDALRaster(ST_SetValues(
    ST_AddBand(ST_MakeEmptyRaster(4, 3, 0, 0, 1, -1, 0, 0, 0), '16BSI'::text, 0, NULL),
    1, 1, 1, ARRAY(SELECT ARRAY(SELECT (base + y * 4 + x)::float8 FROM generate_series(1, 4) x)
                   FROM generate_series(0, 2) y)), 'GTiff'));
  PERFORM lo_export(lo, path);
  PERFORM lo_unlink(lo);
  RETURN base;
END $$;
SELECT 'written', gis_outdb_check.write_tif(:'path', 0);

CREATE TABLE gis_outdb (id int PRIMARY KEY, r raster);
INSERT INTO gis_outdb VALUES
  (1, ST_AddBand(ST_MakeEmptyRaster(4, 3, 0, 0, 1, -1, 0, 0, 0), 1, :'path', ARRAY[1])),
  -- an in-database band beside an out-of-database one
  (2, ST_AddBand(ST_AddBand(ST_MakeEmptyRaster(4, 3, 0, 0, 1, -1, 0, 0, 0), '8BUI'::text, 7, NULL),
                 2, :'path', ARRAY[1]));
-- (in an ordinary schema, so they stay through moves)
CREATE FUNCTION gis_outdb_check.seen(want text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
  RETURN (SELECT string_agg(id || ':' || (ST_BandPath(r, ST_NumBands(r)) = want) || ':'
                    || ST_Value(r, ST_NumBands(r), 2, 2) || ':' || (ST_SummaryStats(r, ST_NumBands(r))).sum,
                    ',' ORDER BY id)
  FROM gis_outdb);
END $$;
SELECT 'reads the file', gis_outdb_check.seen(:'path');
SELECT 'band kinds', string_agg(id || ':' || first.isoutdb || '/' || last.isoutdb, ',' ORDER BY id)
FROM gis_outdb, ST_BandMetaData(r, 1) first, ST_BandMetaData(r, ST_NumBands(r)) last;
SELECT md5(string_agg(id || ':' || encode(ST_AsBinary(r), 'hex'), ',' ORDER BY id)) AS bytes FROM gis_outdb \gset
\set bytes_now 'SELECT md5(string_agg(id || '':'' || encode(ST_AsBinary(r), ''hex''), '','' ORDER BY id)) FROM gis_outdb'
SELECT 'commit', (dolt_commit('-Am', 'gis outdb'))[1] ~ '^[0-9a-f]{40}$';
SELECT dolt_hashof('HEAD') AS c1 \gset

-- moves
SELECT 'branch', (dolt_branch('gis_outdb_side'))[1] = '0';
UPDATE gis_outdb SET r = ST_SetValue(r, 1, 1, 1, 9) WHERE id = 2;
SELECT 'commit edit', (dolt_commit('-am', 'gis outdb edit'))[1] ~ '^[0-9a-f]{40}$';
SELECT dolt_hashof('HEAD') AS c2 \gset
SELECT 'reset back', (dolt_reset('--hard', :'c1'))[1] = '0';
SELECT 'reset: same bytes', (:bytes_now) = :'bytes';
SELECT 'reset: reads the file', gis_outdb_check.seen(:'path');
SELECT 'reset forward', (dolt_reset('--hard', :'c2'))[1] = '0';
SELECT 'checkout', (dolt_checkout('gis_outdb_side'))[1] = '0';
SELECT 'checkout: same bytes', (:bytes_now) = :'bytes';
SELECT 'checkout: reads the file', gis_outdb_check.seen(:'path');
INSERT INTO gis_outdb VALUES (3, ST_AddBand(ST_MakeEmptyRaster(4, 3, 0, 0, 1, -1, 0, 0, 0), 1, :'path', ARRAY[1]));
SELECT 'side commit', (dolt_commit('-am', 'gis outdb side'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';
SELECT 'merge', (dolt_merge('gis_outdb_side'))[2:4];
SELECT 'merged: reads the file', gis_outdb_check.seen(:'path');

-- a clone into another database of this server reads the same file
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/gis-outdb-' || txid_current() AS url \gset
SELECT 'remote', dolt_remote('add', 'outdb_origin', :'url') IS NOT NULL;
SELECT 'push', dolt_push('outdb_origin', 'main') IS NOT NULL;
:bytes_now \gset src_
SELECT current_database() AS db \gset
CREATE DATABASE regress_gis_outdb_clone;
\c regress_gis_outdb_clone
SET client_min_messages = warning;
SET postgis.gdal_enabled_drivers = 'GTiff';
SET postgis.enable_outdb_rasters = true;
SELECT 'clone', dolt_clone(:'url') IS NOT NULL;
:bytes_now \gset clone_
SELECT 'clone: same bytes', :'clone_md5' = :'src_md5';
SELECT 'clone: reads the file', string_agg(id || ':' || (ST_BandPath(r, ST_NumBands(r)) = :'path') || ':'
                    || ST_Value(r, ST_NumBands(r), 2, 2), ',' ORDER BY id) FROM gis_outdb;
\c :db
DROP DATABASE regress_gis_outdb_clone;
SET postgis.gdal_enabled_drivers = 'GTiff';
SET postgis.enable_outdb_rasters = true;

-- the file isn't versioned: rewritten, every commit's rasters read it
SELECT 'rewritten', gis_outdb_check.write_tif(:'path', 100);
SELECT 'now', gis_outdb_check.seen(:'path');
SELECT 'reset to the first commit', (dolt_reset('--hard', :'c1'))[1] = '0';
SELECT 'its bytes', (:bytes_now) = :'bytes';
SELECT 'its pixels: the new file''s', gis_outdb_check.seen(:'path');
SELECT 'nothing to commit', count(*) FROM dolt_status;
