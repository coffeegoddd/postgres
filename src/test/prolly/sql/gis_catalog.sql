--
-- What these tests cover, from the catalog: every type, operator class,
-- operator and aggregate PostGIS and PostGIS raster install, each with
-- what covers it or why nothing does.  A PostGIS that adds one changes
-- this list, and the suite fails until the new member is covered or
-- excluded here with a reason.
--
-- Operators between geometry, geography and raster, and aggregates of one
-- of them, are evaluated here on the corpus as stored: their answers
-- (or errors) must be heap's.
--
-- The samples: for geometry, the corpus's values without an SRID (every
-- type and empty, curves, TINs, NaN and extreme coordinates, values up
-- to a megabyte) and a stride of gis_idx that takes 2D, Z and M points,
-- polygons and a 3D line; for geography, the antimeridian and polar
-- shapes and generated points; every raster.  Aggregates run over each
-- group of a sample (gis_check.groups) alone: most refuse mixed types or
-- dimensions.
CREATE OR REPLACE FUNCTION gis_check.sample(typ regtype) RETURNS text
LANGUAGE sql AS $$
  SELECT CASE
    WHEN typ = 'geometry'::regtype THEN
      'SELECT id, g AS v FROM gis_geoms WHERE ST_SRID(g) = 0 UNION ALL '
      'SELECT 1000 + id, g FROM gis_idx WHERE id % 37 = 1 OR id IN (603, 604)'
    WHEN typ = 'geography'::regtype THEN 'SELECT id, g AS v FROM gis_geogs WHERE id <= 10 OR id % 30 = 1'
    WHEN typ = 'raster'::regtype THEN 'SELECT id, r AS v FROM gis_rasters' END
$$;
CREATE OR REPLACE FUNCTION gis_check.groups(typ regtype) RETURNS text
LANGUAGE sql AS $$
  SELECT CASE typ WHEN 'geometry'::regtype THEN 'GeometryType(v) || ST_NDims(v) || ST_Zmflag(v)'
                  WHEN 'geography'::regtype THEN 'GeometryType(v::geometry) || ST_NDims(v::geometry)'
                  WHEN 'raster'::regtype THEN 'id::text' END
$$;

-- Aggregates whose other arguments need values: a call of each, by
-- signature.
CREATE TABLE gis_check.calls (signature text PRIMARY KEY, query text);
INSERT INTO gis_check.calls VALUES
  ('st_asmvt(anyelement)', $$SELECT ST_AsMVT(t) FROM (SELECT id, ST_AsMVTGeom(g, ST_MakeEnvelope(0, 0, 40, 30)) AS geom FROM gis_idx WHERE id % 37 = 1 OR id IN (603, 604) ORDER BY id) t$$),
  ('st_asmvt(anyelement,text)', $$SELECT ST_AsMVT(t, 'l') FROM (SELECT id, ST_AsMVTGeom(g, ST_MakeEnvelope(0, 0, 40, 30)) AS geom FROM gis_idx WHERE id % 37 = 1 OR id IN (603, 604) ORDER BY id) t$$),
  ('st_asmvt(anyelement,text,integer)', $$SELECT ST_AsMVT(t, 'l', 1024) FROM (SELECT id, ST_AsMVTGeom(g, ST_MakeEnvelope(0, 0, 40, 30)) AS geom FROM gis_idx WHERE id % 37 = 1 OR id IN (603, 604) ORDER BY id) t$$),
  ('st_asmvt(anyelement,text,integer,text)', $$SELECT ST_AsMVT(t, 'l', 1024, 'geom') FROM (SELECT id, ST_AsMVTGeom(g, ST_MakeEnvelope(0, 0, 40, 30)) AS geom FROM gis_idx WHERE id % 37 = 1 OR id IN (603, 604) ORDER BY id) t$$),
  ('st_asmvt(anyelement,text,integer,text,text)', $$SELECT ST_AsMVT(t, 'l', 1024, 'geom', 'id') FROM (SELECT id, ST_AsMVTGeom(g, ST_MakeEnvelope(0, 0, 40, 30)) AS geom FROM gis_idx WHERE id % 37 = 1 OR id IN (603, 604) ORDER BY id) t$$),
  -- (FlatGeobuf takes one type and dimension per file: the 2D points)
  ('st_asflatgeobuf(anyelement)', $$SELECT ST_AsFlatGeobuf(t) FROM (SELECT id, g FROM gis_idx WHERE id % 7 = 0 AND ST_NDims(g) = 2 AND GeometryType(g) = 'POINT' ORDER BY id) t$$),
  ('st_asflatgeobuf(anyelement,boolean)', $$SELECT ST_AsFlatGeobuf(t, false) FROM (SELECT id, g FROM gis_idx WHERE id % 7 = 0 AND ST_NDims(g) = 2 AND GeometryType(g) = 'POINT' ORDER BY id) t$$),
  ('st_asflatgeobuf(anyelement,boolean,text)', $$SELECT ST_AsFlatGeobuf(t, false, 'g') FROM (SELECT id, g FROM gis_idx WHERE id % 7 = 0 AND ST_NDims(g) = 2 AND GeometryType(g) = 'POINT' ORDER BY id) t$$),
  ('st_asgeobuf(anyelement)', $$SELECT ST_AsGeobuf(t) FROM (SELECT id, g FROM gis_idx WHERE id % 37 = 1 OR id IN (603, 604) ORDER BY id) t$$),
  ('st_asgeobuf(anyelement,text)', $$SELECT ST_AsGeobuf(t, 'g') FROM (SELECT id, g FROM gis_idx WHERE id % 37 = 1 OR id IN (603, 604) ORDER BY id) t$$),
  ('st_asrasteragg(geometry,double precision,raster,text,double precision,text,boolean)', $$SELECT ST_AsRasterAgg(g, id, ST_MakeEmptyRaster(40, 30, 0, 30, 1, -1, 0, 0, 0), '32BF', -1, 'MAX', true ORDER BY id) FROM gis_idx WHERE id % 37 = 1 OR id IN (603, 604)$$),
  ('st_clusterwithin(geometry,double precision)', $$SELECT ST_ClusterWithin(g, 5 ORDER BY id) FROM gis_idx WHERE id % 7 = 0 AND ST_NDims(g) = 2 OR id = 603$$),
  ('st_union(geometry,double precision)', $$SELECT ST_Union(g, 0.5 ORDER BY id) FROM gis_idx WHERE id % 7 = 0 AND ST_NDims(g) = 2 OR id = 603$$),
  ('st_countagg(raster,boolean)', $$SELECT ST_CountAgg(r, true ORDER BY id) FROM gis_rasters$$),
  ('st_countagg(raster,integer,boolean)', $$SELECT ST_CountAgg(r, 1, true ORDER BY id) FROM gis_rasters$$),
  ('st_countagg(raster,integer,boolean,double precision)', $$SELECT ST_CountAgg(r, 1, true, 1 ORDER BY id) FROM gis_rasters$$),
  ('st_summarystatsagg(raster,boolean,double precision)', $$SELECT ST_SummaryStatsAgg(r, true, 1 ORDER BY id) FROM gis_rasters$$),
  ('st_summarystatsagg(raster,integer,boolean)', $$SELECT ST_SummaryStatsAgg(r, 1, true ORDER BY id) FROM gis_rasters$$),
  ('st_summarystatsagg(raster,integer,boolean,double precision)', $$SELECT ST_SummaryStatsAgg(r, 1, true, 1 ORDER BY id) FROM gis_rasters$$),
  ('st_union(raster,integer)', $$SELECT ST_Union(r, 1 ORDER BY id) FROM gis_rasters WHERE id <= 5$$),
  ('st_union(raster,integer,text)', $$SELECT ST_Union(r, 1, 'SUM' ORDER BY id) FROM gis_rasters WHERE id <= 5$$),
  ('st_union(raster,text)', $$SELECT ST_Union(r, 'MAX' ORDER BY id) FROM gis_rasters WHERE id <= 5$$),
  ('st_union(raster,unionarg[])', $$SELECT ST_Union(r, ARRAY[ROW(1, 'LAST')]::unionarg[] ORDER BY id) FROM gis_rasters WHERE id <= 5$$);

-- Aggregates evaluated on groups with Z only: ST_3DExtent of values
-- without Z leaves the extent's Z bounds uninitialized (PostGIS's, on
-- heap as well), so its answer changes from run to run.
CREATE TABLE gis_check.z_only (signature text PRIMARY KEY);
INSERT INTO gis_check.z_only VALUES ('st_3dextent(geometry)');

CREATE OR REPLACE FUNCTION gis_check.members()
RETURNS TABLE (kind text, name text, oid oid)
LANGUAGE sql AS $$
  SELECT 'type', t.typname || ' (' || t.typtype::text || ')', t.oid FROM pg_type t
  JOIN pg_depend d ON d.classid = 'pg_type'::regclass AND d.objid = t.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname IN ('postgis', 'postgis_raster')
  UNION ALL
  SELECT 'opclass', a.amname || '/' || c.opcname, c.oid FROM pg_opclass c
  JOIN pg_am a ON a.oid = c.opcmethod
  JOIN pg_depend d ON d.classid = 'pg_opclass'::regclass AND d.objid = c.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname IN ('postgis', 'postgis_raster')
  UNION ALL
  SELECT 'operator', o.oprname || '(' || o.oprleft::regtype || ',' || o.oprright::regtype || ')', o.oid
  FROM pg_operator o
  JOIN pg_depend d ON d.classid = 'pg_operator'::regclass AND d.objid = o.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname IN ('postgis', 'postgis_raster')
  UNION ALL
  SELECT 'aggregate', p.oid::regprocedure::text, p.oid FROM pg_proc p
  JOIN pg_depend d ON d.classid = 'pg_proc'::regclass AND d.objid = p.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname IN ('postgis', 'postgis_raster')
  WHERE p.prokind = 'a'
$$;

-- An operator class is covered by gis_indexes' agreement check
-- (gis_check.index_results) only if some plan used its index.
CREATE OR REPLACE FUNCTION gis_check.coverage()
RETURNS TABLE (kind text, name text, covered_by text)
LANGUAGE sql AS $$
  WITH stored(t) AS (VALUES ('geometry'::regtype), ('geography'::regtype), ('raster'::regtype))
  SELECT m.kind, m.name, CASE m.kind
    WHEN 'type' THEN (SELECT CASE
        WHEN t.typname IN ('box2df', 'gidx') THEN 'excluded: index keys; their input is not implemented, so no value can be stored'
        WHEN t.typtype = 'c' THEN 'excluded: function results, composites of covered types'
        WHEN t.typsend <> 0 THEN 'values (send/recv)'
        ELSE 'values (text form)' END FROM pg_type t WHERE t.oid = m.oid)
    WHEN 'opclass' THEN coalesce((SELECT CASE
        WHEN cardinality(regexp_split_to_array(substring(r.result FROM 'unused (.*)$'), ' '))
             = substring(r.result FROM '\((\d+) operators\)')::int
          THEN 'excluded: no plan uses its index (its operators are SQL functions the planner inlines)'
        ELSE 'index agreement' END FROM gis_check.index_results r WHERE r.opclass = m.name), 'NOT COVERED')
    WHEN 'operator' THEN (SELECT CASE
        WHEN o.oprleft IN (SELECT t FROM stored) AND o.oprright IN (SELECT t FROM stored) THEN 'evaluated'
        WHEN o.oprleft IN ('box2df'::regtype, 'gidx'::regtype) OR o.oprright IN ('box2df'::regtype, 'gidx'::regtype)
          THEN 'excluded: an index key argument'
        ELSE 'NOT COVERED' END FROM pg_operator o WHERE o.oid = m.oid)
    WHEN 'aggregate' THEN (SELECT CASE
        WHEN p.pronargs = 1 AND p.proargtypes[0] IN (SELECT t FROM stored) THEN 'evaluated'
        WHEN m.name IN (SELECT signature FROM gis_check.calls) THEN 'called'
        ELSE 'NOT COVERED' END FROM pg_proc p WHERE p.oid = m.oid) END
  FROM gis_check.members() m
$$;

-- Each operator over every pair of samples (pair by pair, an error its
-- SQLSTATE, when the whole doesn't run), and each aggregate over each
-- group of a sample (a group's error its SQLSTATE), digested.
CREATE OR REPLACE FUNCTION gis_check.evaluate()
RETURNS TABLE (name text, answer text)
LANGUAGE plpgsql AS $$
DECLARE
  m record;
  o record;
  p record;
  pair record;
  parts text[];
  part text;
BEGIN
  FOR m IN SELECT c.name, c.kind, c.covered_by, x.oid FROM gis_check.coverage() c
           JOIN gis_check.members() x ON x.kind = c.kind AND x.name = c.name
           WHERE c.covered_by IN ('evaluated', 'called') ORDER BY c.kind COLLATE "C", c.name COLLATE "C" LOOP
    name := m.kind || ' ' || m.name;
    IF m.covered_by = 'called' THEN
      BEGIN
        EXECUTE format('SELECT left(md5(coalesce(a::text, ''null'')), 12) FROM (%s) s(a)',
                       (SELECT query FROM gis_check.calls WHERE signature = m.name))
          INTO answer;
      EXCEPTION WHEN OTHERS THEN
        answer := 'error ' || SQLSTATE;
      END;
    ELSIF m.kind = 'operator' THEN
      SELECT oprname, oprleft::regtype AS l, oprright::regtype AS r INTO o FROM pg_operator WHERE oid = m.oid;
      BEGIN
        EXECUTE format('SELECT left(md5(string_agg(coalesce((a.v OPERATOR(public.%s) b.v)::text, ''null''), '','''
                       ' ORDER BY a.id, b.id)), 12) FROM (%s) a, (%s) b',
                       o.oprname, gis_check.sample(o.l), gis_check.sample(o.r))
          INTO answer;
      EXCEPTION WHEN OTHERS THEN
        parts := '{}';
        FOR pair IN EXECUTE format('SELECT a.v::text AS a, b.v::text AS b FROM (%s) a, (%s) b ORDER BY a.id, b.id',
                                   gis_check.sample(o.l), gis_check.sample(o.r)) LOOP
          BEGIN
            EXECUTE format('SELECT coalesce(($1::%s OPERATOR(public.%s) $2::%s)::text, ''null'')',
                           o.l, o.oprname, o.r) INTO part USING pair.a, pair.b;
          EXCEPTION WHEN OTHERS THEN
            part := 'error ' || SQLSTATE;
          END;
          parts := parts || part;
        END LOOP;
        answer := 'pairwise ' || left(md5(array_to_string(parts, ',')), 12)
                  || ' (' || (SELECT count(*) FROM unnest(parts) x WHERE x LIKE 'error%') || ' errors)';
      END;
    ELSE
      SELECT proname, proargtypes[0]::regtype AS a INTO p FROM pg_proc WHERE oid = m.oid;
      parts := '{}';
      FOR pair IN EXECUTE format('SELECT DISTINCT %s AS k FROM (%s) s WHERE %s ORDER BY 1',
                                 gis_check.groups(p.a), gis_check.sample(p.a),
                                 CASE WHEN m.name IN (SELECT signature FROM gis_check.z_only)
                                      THEN 'ST_HasZ(v)' ELSE 'true' END) LOOP
        BEGIN
          EXECUTE format('SELECT coalesce(%s(v ORDER BY id)::text, ''null'') FROM (%s) s WHERE %s = $1',
                         m.oid::regproc, gis_check.sample(p.a), gis_check.groups(p.a))
            INTO part USING pair.k;
        EXCEPTION WHEN OTHERS THEN
          part := 'error ' || SQLSTATE;
        END;
        parts := parts || (pair.k || '=' || part);
      END LOOP;
      answer := left(md5(array_to_string(parts, ',')), 12) || ' (' || cardinality(parts) || ' groups, '
                || (SELECT count(*) FROM unnest(parts) x WHERE x LIKE '%=error%') || ' errors)';
    END IF;
    RETURN NEXT;
  END LOOP;
END $$;

SELECT kind, name, covered_by FROM gis_check.coverage() ORDER BY kind COLLATE "C", name COLLATE "C";
CREATE TABLE gis_check.answers AS SELECT * FROM gis_check.evaluate();
SELECT * FROM gis_check.answers ORDER BY name COLLATE "C";
-- (no call may be of nothing: an aggregate of no rows is null)
SELECT name FROM gis_check.answers WHERE answer = left(md5('null'), 12) ORDER BY name COLLATE "C";
