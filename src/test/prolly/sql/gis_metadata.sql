--
-- PostGIS's metadata views list tables of the AM under test, and its
-- column management functions work on them.
--
SELECT f_table_schema, f_table_name, f_geometry_column, coord_dimension, srid, type
FROM geometry_columns WHERE f_table_name LIKE 'gis\_%' ORDER BY 2, 3;
SELECT f_table_schema, f_table_name, f_geography_column, coord_dimension, srid, type
FROM geography_columns WHERE f_table_name LIKE 'gis\_%' ORDER BY 2, 3;

CREATE TABLE gis_managed (id int PRIMARY KEY);
SELECT AddGeometryColumn('gis_managed', 'pt', 4326, 'POINT', 2);
SELECT AddGeometryColumn('gis_managed', 'area', 3857, 'MULTIPOLYGONM', 3, false);
INSERT INTO gis_managed VALUES (1, 'SRID=4326;POINT(1 1)', NULL);
INSERT INTO gis_managed VALUES (2, 'SRID=4326;LINESTRING(1 1, 2 2)', NULL);
SELECT f_geometry_column, coord_dimension, srid, type FROM geometry_columns
WHERE f_table_name = 'gis_managed' ORDER BY 1;
SELECT DropGeometryColumn('gis_managed', 'area');
SELECT attname FROM pg_attribute WHERE attrelid = 'gis_managed'::regclass AND attnum > 0
  AND NOT attisdropped ORDER BY attnum;

-- an unconstrained column, given a typmod from its values
CREATE TABLE gis_loose (id int PRIMARY KEY, g geometry);
INSERT INTO gis_loose VALUES (1, 'SRID=4326;POINT(1 2)'), (2, 'SRID=4326;POINT(3 4)');
SELECT Populate_Geometry_Columns('gis_loose'::regclass);
SELECT format_type(atttypid, atttypmod) FROM pg_attribute
WHERE attrelid = 'gis_loose'::regclass AND attname = 'g';

-- rasters with their constraints
CREATE TABLE gis_tiles AS SELECT id, r AS rast FROM gis_rasters WHERE id <= 5;
SELECT AddRasterConstraints('gis_tiles'::name, 'rast'::name, 'srid', 'num_bands', 'pixel_types');
SELECT r_table_name, r_raster_column, srid, num_bands, pixel_types FROM raster_columns
WHERE r_table_name = 'gis_tiles';
INSERT INTO gis_tiles SELECT 99, r FROM gis_rasters WHERE id = 6;
SELECT count(*) FROM gis_tiles;
