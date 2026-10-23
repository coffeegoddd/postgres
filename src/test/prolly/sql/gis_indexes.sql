--
-- Every PostGIS operator class indexes a table of the AM under test as it
-- does heap: for each of its operators, index and bitmap scans find what a
-- sequential scan finds, and KNN gives the same distances in order.
--
-- (SP-GiST's 2D class misses rows on the boundary for &>, which a
-- sequential scan finds: PostGIS's, on heap as well.)
--
SELECT * FROM gis_check.index_agreement('gis_idx');
SELECT * FROM gis_check.index_agreement('gis_geogs');
SELECT * FROM gis_check.index_agreement('gis_rasters');

-- an exclusion constraint on a GiST index
CREATE TABLE gis_parcels (id int PRIMARY KEY, g geometry,
  EXCLUDE USING gist (g WITH &&));
INSERT INTO gis_parcels VALUES (1, 'POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))'),
  (2, 'POLYGON((2 2, 3 2, 3 3, 2 3, 2 2))');
INSERT INTO gis_parcels VALUES (3, 'POLYGON((0.5 0.5, 1.5 0.5, 1.5 1.5, 0.5 0.5))');
SELECT count(*) FROM gis_parcels;

-- statistics: PostGIS's own, and the estimates and plans made from them
ANALYZE gis_idx, gis_geogs, gis_geoms;
SELECT stakind1, stakind2, stakind3, stakind4, stakind5 FROM pg_statistic s JOIN pg_attribute a
  ON a.attrelid = s.starelid AND a.attnum = s.staattnum
WHERE starelid = 'gis_idx'::regclass AND attname = 'g' AND NOT stainherit;
SELECT ST_AsText(ST_EstimatedExtent('gis_idx', 'g'));
SELECT ST_AsText(ST_EstimatedExtent('public', 'gis_geogs', 'g'));
CREATE INDEX gis_idx_g ON gis_idx USING gist (g);
CREATE INDEX gis_geogs_g ON gis_geogs USING gist (g);
ANALYZE gis_idx, gis_geogs;
EXPLAIN (COSTS OFF) SELECT count(*) FROM gis_idx WHERE g && ST_MakeEnvelope(0, 0, 2, 2);
EXPLAIN (COSTS OFF) SELECT count(*) FROM gis_idx WHERE ST_Intersects(g, ST_MakeEnvelope(0, 0, 2, 2));
EXPLAIN (COSTS OFF) SELECT id FROM gis_idx ORDER BY g <-> 'POINT(3 3)'::geometry LIMIT 5;
EXPLAIN (COSTS OFF) SELECT count(*) FROM gis_geogs WHERE ST_DWithin(g, 'POINT(0 0)'::geography, 100000);
SELECT count(*) FROM gis_idx WHERE ST_Intersects(g, ST_MakeEnvelope(0, 0, 2, 2));
SELECT array_agg(round(d::numeric, 6) ORDER BY d) FROM (SELECT g <-> 'POINT(3 3)'::geometry AS d
  FROM gis_idx ORDER BY d LIMIT 5) s;
SELECT count(*) FROM gis_geogs WHERE ST_DWithin(g, 'POINT(0 0)'::geography, 2000000);
