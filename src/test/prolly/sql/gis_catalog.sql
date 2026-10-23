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
CREATE OR REPLACE FUNCTION gis_check.sample(typ regtype) RETURNS text
LANGUAGE sql AS $$
  SELECT CASE typ WHEN 'geometry'::regtype THEN 'SELECT id, g AS v FROM gis_idx WHERE id % 40 = 1'
                  WHEN 'geography'::regtype THEN 'SELECT id, g AS v FROM gis_geogs WHERE id % 30 = 1'
                  WHEN 'raster'::regtype THEN 'SELECT id, r AS v FROM gis_rasters' END
$$;

-- Aggregates whose other arguments need values: a call of each, by
-- signature.
CREATE TABLE gis_check.calls (signature text PRIMARY KEY, query text);
INSERT INTO gis_check.calls VALUES
  ('st_asmvt(anyelement)', $$SELECT ST_AsMVT(t) FROM (SELECT id, ST_AsMVTGeom(g, ST_MakeEnvelope(0, 0, 40, 30)) AS geom FROM gis_idx WHERE id % 40 = 1 ORDER BY id) t$$),
  ('st_asmvt(anyelement,text)', $$SELECT ST_AsMVT(t, 'l') FROM (SELECT id, ST_AsMVTGeom(g, ST_MakeEnvelope(0, 0, 40, 30)) AS geom FROM gis_idx WHERE id % 40 = 1 ORDER BY id) t$$),
  ('st_asmvt(anyelement,text,integer)', $$SELECT ST_AsMVT(t, 'l', 1024) FROM (SELECT id, ST_AsMVTGeom(g, ST_MakeEnvelope(0, 0, 40, 30)) AS geom FROM gis_idx WHERE id % 40 = 1 ORDER BY id) t$$),
  ('st_asmvt(anyelement,text,integer,text)', $$SELECT ST_AsMVT(t, 'l', 1024, 'geom') FROM (SELECT id, ST_AsMVTGeom(g, ST_MakeEnvelope(0, 0, 40, 30)) AS geom FROM gis_idx WHERE id % 40 = 1 ORDER BY id) t$$),
  ('st_asmvt(anyelement,text,integer,text,text)', $$SELECT ST_AsMVT(t, 'l', 1024, 'geom', 'id') FROM (SELECT id, ST_AsMVTGeom(g, ST_MakeEnvelope(0, 0, 40, 30)) AS geom FROM gis_idx WHERE id % 40 = 1 ORDER BY id) t$$),
  ('st_asflatgeobuf(anyelement)', $$SELECT ST_AsFlatGeobuf(t) FROM (SELECT id, g FROM gis_idx WHERE id % 40 = 1 AND ST_NDims(g) = 2 ORDER BY id) t$$),
  ('st_asflatgeobuf(anyelement,boolean)', $$SELECT ST_AsFlatGeobuf(t, false) FROM (SELECT id, g FROM gis_idx WHERE id % 40 = 1 AND ST_NDims(g) = 2 ORDER BY id) t$$),
  ('st_asflatgeobuf(anyelement,boolean,text)', $$SELECT ST_AsFlatGeobuf(t, false, 'g') FROM (SELECT id, g FROM gis_idx WHERE id % 40 = 1 AND ST_NDims(g) = 2 ORDER BY id) t$$),
  ('st_asgeobuf(anyelement)', $$SELECT ST_AsGeobuf(t) FROM (SELECT id, g FROM gis_idx WHERE id % 40 = 1 ORDER BY id) t$$),
  ('st_asgeobuf(anyelement,text)', $$SELECT ST_AsGeobuf(t, 'g') FROM (SELECT id, g FROM gis_idx WHERE id % 40 = 1 ORDER BY id) t$$),
  ('st_asrasteragg(geometry,double precision,raster,text,double precision,text,boolean)', $$SELECT ST_AsRasterAgg(g, id, ST_MakeEmptyRaster(40, 30, 0, 30, 1, -1, 0, 0, 0), '32BF', -1, 'MAX', true ORDER BY id) FROM gis_idx WHERE id % 40 = 1$$),
  ('st_clusterwithin(geometry,double precision)', $$SELECT ST_ClusterWithin(g, 5 ORDER BY id) FROM gis_idx WHERE id % 40 = 1$$),
  ('st_union(geometry,double precision)', $$SELECT ST_Union(g, 0.5 ORDER BY id) FROM gis_idx WHERE id % 40 = 1$$),
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
    WHEN 'opclass' THEN (SELECT CASE WHEN c.opcintype IN (SELECT t FROM stored)
        THEN 'index agreement' ELSE 'NOT COVERED' END FROM pg_opclass c WHERE c.oid = m.oid)
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

-- Each operator over pairs of samples, and each aggregate over a sample,
-- digested; an error is its SQLSTATE.
CREATE OR REPLACE FUNCTION gis_check.evaluate()
RETURNS TABLE (name text, answer text)
LANGUAGE plpgsql AS $$
DECLARE
  m record;
  o record;
  p record;
BEGIN
  FOR m IN SELECT c.name, c.kind, c.covered_by, x.oid FROM gis_check.coverage() c
           JOIN gis_check.members() x ON x.kind = c.kind AND x.name = c.name
           WHERE c.covered_by IN ('evaluated', 'called') ORDER BY c.kind, c.name LOOP
    name := m.kind || ' ' || m.name;
    BEGIN
      IF m.covered_by = 'called' THEN
        EXECUTE format('SELECT left(md5(coalesce(a::text, ''null'')), 12) FROM (%s) s(a)',
                       (SELECT query FROM gis_check.calls WHERE signature = m.name))
          INTO answer;
      ELSIF m.kind = 'operator' THEN
        SELECT oprname, oprleft::regtype AS l, oprright::regtype AS r INTO o FROM pg_operator WHERE oid = m.oid;
        EXECUTE format('SELECT left(md5(string_agg(coalesce((a.v OPERATOR(public.%s) b.v)::text, ''null''), '','''
                       ' ORDER BY a.id, b.id)), 12) FROM (%s) a, (%s) b',
                       o.oprname, gis_check.sample(o.l), gis_check.sample(o.r))
          INTO answer;
      ELSE
        SELECT proname, proargtypes[0]::regtype AS a INTO p FROM pg_proc WHERE oid = m.oid;
        EXECUTE format('SELECT left(md5(coalesce(%s(v ORDER BY id)::text, ''null'')), 12) FROM (%s) s',
                       m.oid::regproc, gis_check.sample(p.a))
          INTO answer;
      END IF;
    EXCEPTION WHEN OTHERS THEN
      answer := 'error ' || SQLSTATE;
    END;
    RETURN NEXT;
  END LOOP;
END $$;

SELECT kind, name, covered_by FROM gis_check.coverage() ORDER BY kind, name;
SELECT * FROM gis_check.evaluate();
