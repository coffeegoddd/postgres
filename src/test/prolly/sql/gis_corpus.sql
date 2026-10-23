--
-- Included by the PostGIS tests: a fixed corpus, in the current schema,
-- and the checks every test applies to it, in the ordinary schema
-- gis_check (so they survive moves between commits).
--
--   gis_geoms   every geometry type, dimension and kind of empty, a NaN
--               point, extreme and denormal coordinates, curves, TINs and
--               polyhedral surfaces, and values from a few bytes to over a
--               megabyte (inline, compressed and out of line);
--   gis_geogs   geography, across the antimeridian and around the poles
--               (no empties either);
--   gis_idx     one SRID, 2D to 4D, for indexes (operators refuse mixed
--               SRIDs), and no empties, which SP-GiST's 3D and N-D
--               classes can't index (on heap either);
--   gis_rasters in-database rasters.
--
-- Nothing is random: generated values come from generate_series.
--
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS postgis_raster;
CREATE SCHEMA IF NOT EXISTS gis_check;
RESET client_min_messages;

CREATE TABLE gis_geoms (id int PRIMARY KEY, g geometry);
INSERT INTO gis_geoms VALUES
  (1, 'POINT(1 2)'), (2, 'POINT Z (1 2 3)'), (3, 'POINT M (1 2 4)'), (4, 'POINT ZM (1 2 3 4)'),
  (5, 'SRID=4326;POINT(-71.06 42.36)'), (6, 'SRID=3857;POINT(-7907972 5214990)'),
  (7, 'LINESTRING(0 0, 1 1, 2 0)'), (8, 'LINESTRING ZM (0 0 0 0, 1 1 1 1)'),
  (9, 'POLYGON((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))'),
  (10, 'MULTIPOINT((0 0), (1 1))'), (11, 'MULTILINESTRING((0 0, 1 1), (2 2, 3 3))'),
  (12, 'MULTIPOLYGON(((0 0, 1 0, 1 1, 0 0)), ((5 5, 6 5, 6 6, 5 5)))'),
  (13, 'GEOMETRYCOLLECTION(POINT(1 1), LINESTRING(0 0, 2 2), POLYGON((0 0, 1 0, 1 1, 0 0)))'),
  (14, 'CIRCULARSTRING(0 0, 1 1, 2 0)'),
  (15, 'COMPOUNDCURVE(CIRCULARSTRING(0 0, 1 1, 1 0), (1 0, 0 1))'),
  (16, 'CURVEPOLYGON(CIRCULARSTRING(0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 3 3, 3 1, 1 1))'),
  (17, 'MULTICURVE((0 0, 5 5), CIRCULARSTRING(4 0, 4 4, 8 4))'),
  (18, 'MULTISURFACE(CURVEPOLYGON(CIRCULARSTRING(0 0, 4 0, 4 4, 0 4, 0 0)), ((10 10, 14 12, 11 10, 10 10)))'),
  (19, 'TRIANGLE((0 0, 0 9, 9 0, 0 0))'),
  (20, 'TIN(((0 0 0, 0 0 1, 0 1 0, 0 0 0)), ((0 0 0, 0 1 0, 1 1 0, 0 0 0)))'),
  (21, 'POLYHEDRALSURFACE Z (((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)), ((0 0 0, 0 0 1, 0 1 1, 0 1 0, 0 0 0)))'),
  (22, 'POINT EMPTY'), (23, 'LINESTRING EMPTY'), (24, 'POLYGON EMPTY'), (25, 'MULTIPOINT EMPTY'),
  (26, 'MULTILINESTRING EMPTY'), (27, 'MULTIPOLYGON EMPTY'), (28, 'GEOMETRYCOLLECTION EMPTY'),
  (29, 'GEOMETRYCOLLECTION(POINT EMPTY, LINESTRING(0 0, 1 1))'), (30, 'POINT Z EMPTY'),
  (31, ST_MakePoint('NaN', 'NaN')),
  (32, 'POINT(1e308 -1e308)'), (33, 'POINT(5e-324 -5e-324)'), (34, 'POINT(0 -0)'),
  (35, 'SRID=4326;MULTIPOINT Z ((1 2 3), (4 5 6))'),
  (36, NULL);
-- sizes: a few hundred bytes, tens of kilobytes (out of line), over a
-- megabyte uncompressed (its own value tree in a commit)
INSERT INTO gis_geoms
SELECT 36 + n, ST_MakeLine(array_agg(ST_MakePoint(i * 0.5, sin(i * 0.37) * 100 + n) ORDER BY i))
FROM (VALUES (1, 20), (2, 2000)) v(n, points), generate_series(1, points) i GROUP BY n;
INSERT INTO gis_geoms VALUES (39, ST_Buffer('POINT(0 0)'::geometry, 1000, 20000));

CREATE TABLE gis_geogs (id int PRIMARY KEY, g geography);
INSERT INTO gis_geogs VALUES
  (1, 'POINT(-71.06 42.36)'), (2, 'POINT(179.99 0)'), (3, 'POINT(-179.99 0)'),
  (4, 'POINT(0 90)'), (5, 'POINT(0 -90)'),
  (6, 'LINESTRING(170 10, -170 10)'),
  (7, 'POLYGON((175 -5, -175 -5, -175 5, 175 5, 175 -5))'),
  (8, 'POLYGON((0 80, 90 80, 180 80, -90 80, 0 80))'),
  (10, 'POINT Z (10 20 30)');
INSERT INTO gis_geogs
SELECT 10 + i, ST_MakePoint((i * 37) % 360 - 180, (i * 13) % 180 - 90)::geography
FROM generate_series(1, 300) i;

CREATE TABLE gis_idx (id int PRIMARY KEY, g geometry);
INSERT INTO gis_idx
SELECT i, CASE i % 4
  WHEN 0 THEN ST_MakePoint(i % 37, i % 23)
  WHEN 1 THEN ST_MakePoint(i % 37, i % 23, i % 11)
  WHEN 2 THEN ST_MakePointM(i % 37, i % 23, i % 7)
  ELSE ST_Expand(ST_MakePoint(i % 31, i % 19), i % 3 + 0.5) END
FROM generate_series(1, 600) i;
INSERT INTO gis_idx VALUES (602, NULL),
  (603, 'POLYGON((0 0, 40 0, 40 30, 0 30, 0 0))'), (604, 'LINESTRING Z (0 0 0, 30 20 10)');

CREATE TABLE gis_rasters (id int PRIMARY KEY, r raster);
INSERT INTO gis_rasters
SELECT i, ST_AddBand(ST_MakeEmptyRaster(4 + i, 3 + i, i, -i, 1, -1, 0, 0, 4326),
                     '16BSI'::text, i * 7, NULL)
FROM generate_series(1, 5) i;
-- (a band of distinct values, and a large raster stored out of line)
INSERT INTO gis_rasters
SELECT 6, ST_SetValues(ST_AddBand(ST_MakeEmptyRaster(10, 10, 0, 0, 1, -1, 0, 0, 0), '32BF'::text, 0, NULL),
                       1, 1, 1, ARRAY(SELECT ARRAY(SELECT (y * 10 + x)::float8 / 3 FROM generate_series(1, 10) x)
                                      FROM generate_series(1, 10) y));
INSERT INTO gis_rasters
SELECT 7, ST_AddBand(ST_MakeEmptyRaster(300, 300, 0, 0, 1, -1, 0, 0, 0), '64BF'::text, 1.5, NULL);

-- Each table's values, exactly: EWKB (or WKB) bytes in id order.
CREATE OR REPLACE FUNCTION gis_check.digest(tbl regclass) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
  col name;
  typ regtype;
  out text;
BEGIN
  SELECT attname, atttypid::regtype INTO col, typ FROM pg_attribute
  WHERE attrelid = tbl AND attnum = 2;
  EXECUTE format($q$SELECT md5(string_agg(id || ':' || coalesce(%s, 'null'), ',' ORDER BY id))
                    FROM %s$q$,
                 CASE typ WHEN 'raster'::regtype THEN format('encode(ST_AsBinary(%I), ''hex'')', col)
                          WHEN 'geometry'::regtype THEN format('encode(ST_AsEWKB(%I), ''hex'')', col)
                          -- (geography's text form is its hex EWKB)
                          ELSE format('%I::text', col) END, tbl)
  INTO out;
  RETURN out;
END $$;

-- An index of the table against sequential scans: for each operator of
-- the index's operator family over the column's type, the index scan and
-- the bitmap scan must find what a sequential scan finds (an ordering
-- operator: the same distances in the same order, as KNN), for probes
-- taken from the table.  "unused" names operators whose plan didn't use
-- the index.
CREATE OR REPLACE FUNCTION gis_check.scans_agree(tbl regclass, idx regclass) RETURNS text
LANGUAGE plpgsql
SET enable_seqscan = on SET enable_indexscan = on SET enable_bitmapscan = on
AS $$
DECLARE
  col name;
  typ regtype;
  am name;
  family oid;
  op record;
  p record;
  q text;
  results text[];
  d text;
  plan text;
  line text;
  bad text[] := '{}';
  unused text[] := '{}';
  ops int := 0;
BEGIN
  SELECT attname, atttypid::regtype INTO col, typ FROM pg_attribute
  WHERE attrelid = tbl AND attnum = 2;
  SELECT a.amname, c.opcfamily INTO am, family FROM pg_index i
  JOIN pg_opclass c ON c.oid = i.indclass[0] JOIN pg_am a ON a.oid = c.opcmethod
  WHERE i.indexrelid = idx;
  FOR op IN
    SELECT DISTINCT o.oprname, ao.amoppurpose FROM pg_amop ao JOIN pg_operator o ON o.oid = ao.amopopr
    WHERE ao.amopfamily = family AND ao.amoplefttype = typ AND ao.amoprighttype = typ
    ORDER BY 1, 2
  LOOP
    ops := ops + 1;
    IF op.amoppurpose = 's' THEN
      q := format('SELECT count(*)::text FROM %s WHERE %I %s $1::%s', tbl, col, op.oprname, typ);
    ELSE
      q := format('SELECT array_agg(d)::text FROM (SELECT round((%I %s $1::%s)::numeric, 6) AS d'
                  ' FROM %s WHERE %I IS NOT NULL ORDER BY %I %s $1::%s LIMIT 20) s',
                  col, op.oprname, typ, tbl, col, col, op.oprname, typ);
    END IF;
    FOR p IN EXECUTE format('SELECT %I::text AS v FROM %s WHERE id %% 50 = 3 AND %I IS NOT NULL ORDER BY id',
                            col, tbl, col)
    LOOP
      results := '{}';
      FOR i IN 1..3 LOOP
        PERFORM set_config('enable_seqscan', CASE WHEN i = 1 THEN 'on' ELSE 'off' END, true);
        PERFORM set_config('enable_indexscan', CASE WHEN i = 2 THEN 'on' ELSE 'off' END, true);
        PERFORM set_config('enable_bitmapscan', CASE WHEN i = 3 THEN 'on' ELSE 'off' END, true);
        BEGIN
          EXECUTE q INTO d USING p.v;
        EXCEPTION WHEN OTHERS THEN
          -- (an operator may refuse a pair, M on one side only: every
          -- scan must refuse it alike)
          d := 'error ' || SQLSTATE;
        END;
        results := results || d;
      END LOOP;
      -- (BRIN has bitmap scans only; ordering operators index scans only)
      IF results[1] IS DISTINCT FROM results[2] AND am <> 'brin'
         OR results[1] IS DISTINCT FROM results[3] AND op.amoppurpose = 's' THEN
        bad := array_append(bad, op.oprname);
        EXIT;
      END IF;
    END LOOP;
    PERFORM set_config('enable_seqscan', 'off', true);
    PERFORM set_config('enable_indexscan', 'on', true);
    PERFORM set_config('enable_bitmapscan', 'on', true);
    plan := '';
    FOR p IN EXECUTE format('SELECT %I::text AS v FROM %s WHERE %I IS NOT NULL ORDER BY id LIMIT 1',
                            col, tbl, col)
    LOOP
      FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q USING p.v LOOP
        plan := plan || line || ' ';
      END LOOP;
    END LOOP;
    IF position(idx::text IN plan) = 0 THEN
      unused := array_append(unused, op.oprname);
    END IF;
  END LOOP;
  RETURN CASE WHEN bad = '{}' THEN 'agree' ELSE 'DIFFER ' || array_to_string(bad, ' ') END
         || ' (' || ops || ' operators)'
         || CASE WHEN unused = '{}' THEN '' ELSE ' unused ' || array_to_string(unused, ' ') END;
END $$;

-- Every PostGIS operator class over the column's type, catalog-enumerated:
-- an index of it on the table, checked by scans_agree.  (Operators with
-- an index key argument type aren't checked: no value of one can be
-- made.)
CREATE OR REPLACE FUNCTION gis_check.index_agreement(tbl regclass)
RETURNS TABLE (opclass text, result text)
LANGUAGE plpgsql AS $$
DECLARE
  col name;
  typ regtype;
  oc record;
BEGIN
  SELECT attname, atttypid::regtype INTO col, typ FROM pg_attribute
  WHERE attrelid = tbl AND attnum = 2;
  FOR oc IN
    SELECT c.opcname, a.amname FROM pg_opclass c
    JOIN pg_am a ON a.oid = c.opcmethod
    JOIN pg_depend d ON d.classid = 'pg_opclass'::regclass AND d.objid = c.oid AND d.deptype = 'e'
    WHERE c.opcintype = typ ORDER BY a.amname, c.opcname
  LOOP
    opclass := oc.amname || '/' || oc.opcname;
    BEGIN
      EXECUTE format('CREATE INDEX gis_check_idx ON %s USING %s (%I %I)', tbl, oc.amname, col, oc.opcname);
    EXCEPTION WHEN OTHERS THEN
      result := 'index refused: ' || SQLSTATE;
      RETURN NEXT;
      CONTINUE;
    END;
    result := gis_check.scans_agree(tbl, 'gis_check_idx'::regclass);
    EXECUTE 'DROP INDEX gis_check_idx';
    RETURN NEXT;
  END LOOP;
END $$;

-- A statement's outcome: "accepted", or the error it raised (and undid).
CREATE OR REPLACE FUNCTION gis_check.outcome(q text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE q;
  RETURN 'accepted';
EXCEPTION WHEN OTHERS THEN
  RETURN SQLSTATE || ' ' || SQLERRM;
END $$;
