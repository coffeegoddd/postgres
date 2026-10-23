--
-- PostGIS types without binary send/recv in versioned tables: raster and
-- spheroid are kept as their text forms and come back byte-identical
-- through commits, moves and merges, alone and in arrays, domains and
-- composites; box2d and box3d values whose text doesn't read back the same
-- are refused where they're written, never at a commit; box2df and gidx,
-- index keys, can hold no value.  And a NaN point comes back from a
-- commit as POINT EMPTY, as through pg_dump: its EWKB is an empty
-- point's.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

SELECT 'on main', active_branch() = 'main';
SELECT 'without send/recv', string_agg(t.typname, ',' ORDER BY t.typname) FROM pg_type t
JOIN pg_depend d ON d.classid = 'pg_type'::regclass AND d.objid = t.oid AND d.deptype = 'e'
WHERE t.typtype = 'b' AND (t.typsend = 0 OR t.typreceive = 0);

CREATE DOMAIN gis_rdom AS raster;
CREATE TYPE gis_band AS (name text, r raster);
CREATE TABLE gis_nosend (id int PRIMARY KEY, r raster, s spheroid, b2 box2d, b3 box3d,
  ra raster[], rd gis_rdom, rc gis_band, ba rastbandarg);
INSERT INTO gis_nosend
SELECT id, r, 'SPHEROID["WGS 84",6378137,298.257223563]', 'BOX(0 0,1 1)', 'BOX3D(0 0 0,1 1 1)',
       ARRAY[r, r], r, ROW('band ' || id, r)::gis_band, ROW(r, 1)::rastbandarg
FROM gis_rasters WHERE id IN (1, 6, 7);
CREATE FUNCTION gis_check.nosend() RETURNS text LANGUAGE sql AS $$
  SELECT md5(string_agg(id || ':' || encode(ST_AsBinary(r), 'hex') || s::text || b2::text || b3::text
                        || array_length(ra, 1) || encode(ST_AsBinary(ra[2]), 'hex')
                        || encode(ST_AsBinary(rd), 'hex') || (rc).name || encode(ST_AsBinary((rc).r), 'hex')
                        || encode(ST_AsBinary((ba).rast), 'hex'), ',' ORDER BY id))
  FROM gis_nosend
$$;
SELECT gis_check.nosend() AS ns1 \gset
SELECT 'status', string_agg(table_name || ' ' || status, ',') FROM dolt_status
WHERE table_name = 'public.gis_nosend';
SELECT 'commit', (dolt_commit('-Am', 'without send/recv'))[1] ~ '^[0-9a-f]{40}$';
DROP TABLE gis_nosend;
SELECT 'reset', (dolt_reset('--hard'))[1] = '0';
SELECT 'reset: same', gis_check.nosend() = :'ns1';
SELECT 'reset: raster reads', string_agg(ST_Value(r, 1, 1, 1)::text, ',' ORDER BY id) FROM gis_nosend;
SELECT 'branch', (dolt_branch('gis_ns'))[1] = '0';
UPDATE gis_nosend SET r = ST_SetValue(r, 1, 1, 1, 5), s = 'SPHEROID["GRS_1980",6378137,298.257222101]'
WHERE id = 1;
SELECT 'commit main', (dolt_commit('-Am', 'main side'))[1] ~ '^[0-9a-f]{40}$';
SELECT gis_check.nosend() AS ns2 \gset
SELECT 'checkout', (dolt_checkout('gis_ns'))[1] = '0';
SELECT 'checkout: same', gis_check.nosend() = :'ns1';
UPDATE gis_nosend SET b2 = 'BOX(2 2,3 3)' WHERE id = 6;
SELECT 'commit side', (dolt_commit('-Am', 'other side'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'merge', (dolt_merge('main'))[2:4];
SELECT 'merged', string_agg(id || ':' || ST_Value(r, 1, 1, 1) || ':' || (s::text LIKE '%GRS_1980%') || ':' || b2,
                            ',' ORDER BY id) FROM gis_nosend;
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';

-- extents PostGIS prints at less than full precision can't be kept
SELECT 'extent refused', gis_check.outcome($$INSERT INTO gis_nosend (id, b2)
  SELECT 10, ST_Extent(g) FROM gis_idx$$);
SELECT 'extent 3D', gis_check.outcome($$INSERT INTO gis_nosend (id, b3)
  SELECT 11, ST_3DExtent(g) FROM gis_idx$$);
SELECT 'status after', count(*) FROM dolt_status;
-- index keys: a column, but no value
CREATE TABLE gis_keys_only (id int PRIMARY KEY, k box2df, x gidx);
SELECT 'box2df value', gis_check.outcome($$INSERT INTO gis_keys_only VALUES (1, 'BOX2DF(0 0, 1 1)', NULL)$$);
SELECT 'gidx value', gis_check.outcome($$INSERT INTO gis_keys_only VALUES (2, NULL, 'GIDX((0 0, 1 1))')$$);
INSERT INTO gis_keys_only VALUES (3, NULL, NULL);
SELECT 'nulls commit', (dolt_commit('-Am', 'index key types'))[1] ~ '^[0-9a-f]{40}$';

-- NaN points
CREATE TABLE gis_nan (id int PRIMARY KEY, g geometry);
INSERT INTO gis_nan VALUES (1, ST_MakePoint('NaN', 'NaN')), (2, ST_MakePoint(-'NaN'::float8, -'NaN'::float8)),
  (3, 'POINT EMPTY'), (4, 'POINT(1 2)');
SELECT 'before', string_agg(id || ':' || ST_IsEmpty(g) || ':' || encode(ST_AsEWKB(g), 'hex'), ',' ORDER BY id)
FROM gis_nan;
SELECT 'commit nan', (dolt_commit('-Am', 'nan points'))[1] ~ '^[0-9a-f]{40}$';
DROP TABLE gis_nan;
SELECT 'reset', (dolt_reset('--hard'))[1] = '0';
SELECT 'after', string_agg(id || ':' || ST_IsEmpty(g) || ':' || encode(ST_AsEWKB(g), 'hex'), ',' ORDER BY id)
FROM gis_nan;
