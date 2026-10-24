--
-- PostGIS through a file remote: a clone into an empty database installs
-- PostGIS in the versioned public it had, with the custom SRID the commits
-- record, and every table's values come back byte-identical, its indexes
-- agreeing with sequential scans; a change pushed is fetched and pulled
-- the same.  A clone into a database
-- whose PostGIS is in another schema is refused.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

SELECT 'on main', active_branch() = 'main';
SELECT current_database() AS db \gset
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/gis-' || txid_current() AS url \gset

-- The digests a database without gis_check can compute too.
\set digests 'SELECT string_agg(t || ''='' || d, '','' ORDER BY t) FROM (SELECT ''geoms'' AS t, md5(string_agg(id || '':'' || coalesce(encode(ST_AsEWKB(g), ''hex''), ''null''), '','' ORDER BY id)) AS d FROM gis_geoms UNION ALL SELECT ''geogs'', md5(string_agg(id || '':'' || coalesce(g::text, ''null''), '','' ORDER BY id)) FROM gis_geogs UNION ALL SELECT ''rasters'', md5(string_agg(id || '':'' || coalesce(encode(ST_AsBinary(r), ''hex''), ''null''), '','' ORDER BY id)) FROM gis_rasters UNION ALL SELECT ''nosend'', md5(string_agg(id || '':'' || coalesce(encode(ST_AsBinary(r), ''hex''), ''null'') || coalesce(s::text, ''''), '','' ORDER BY id)) FROM gis_nosend) s'
-- a custom SRID (a row of spatial_ref_sys, PostGIS's configuration
-- table) a versioned column uses: the commit records it, and the clone
-- gets it
INSERT INTO spatial_ref_sys (srid, auth_name, auth_srid, srtext, proj4text)
SELECT 990001, 'regress', 990001, srtext, proj4text FROM spatial_ref_sys WHERE srid = 3857;
CREATE TABLE gis_custom (id int PRIMARY KEY, g geometry(Point, 990001));
INSERT INTO gis_custom VALUES (1, 'SRID=990001;POINT(1000000 2000000)');
SELECT ST_AsText(ST_SnapToGrid(ST_Transform(g, 4326), 1e-9)) AS custom_4326 FROM gis_custom \gset
SELECT 'custom srid commit', (dolt_commit('-Am', 'gis custom srid'))[1] ~ '^[0-9a-f]{40}$';
:digests \gset src_
SELECT 'remote', dolt_remote('add', 'origin', :'url') IS NOT NULL;
SELECT 'push', dolt_push('origin', 'main') IS NOT NULL;

CREATE DATABASE regress_gis_clone;
\c regress_gis_clone
SELECT 'clone', dolt_clone(:'url') IS NOT NULL;
SELECT 'public versioned', nspversioned FROM pg_namespace WHERE nspname = 'public';
SELECT 'postgis installed', string_agg(extname || ' in ' || extnamespace::regnamespace, ',' ORDER BY extname)
FROM pg_extension WHERE extname LIKE 'postgis%';
:digests \gset clone_
SELECT 'clone: same', :'clone_string_agg' = :'src_string_agg';
SELECT 'clone: custom srid', count(*) FROM spatial_ref_sys WHERE srid = 990001 AND auth_name = 'regress';
SELECT 'clone: transformed as on the source',
  ST_AsText(ST_SnapToGrid(ST_Transform(g, 4326), 1e-9)) = :'custom_4326' FROM gis_custom;
SELECT 'clone: tables on prolly', bool_and(amname = 'prolly') FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE relname LIKE 'gis\_%' AND relkind = 'r' AND relnamespace = 'public'::regnamespace;
-- the GiST index came with it, valid, and a plan uses it
SELECT 'clone: index valid', indisvalid AND indisready FROM pg_index
WHERE indrelid = 'gis_m_gist'::regclass AND NOT indisprimary;
CREATE FUNCTION pg_temp.plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text; out text := '';
BEGIN
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP out := out || line || ' '; END LOOP;
  RETURN out;
END $$;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT 'clone: plan uses it',
  pg_temp.plan($$SELECT count(*) FROM gis_m_gist WHERE g && ST_MakeEnvelope(0, 0, 5, 5)$$)
  LIKE '%Index Scan using gis_m_gist_g_idx on gis_m_gist%';
SELECT 'clone: index', count(*) FROM gis_m_gist WHERE g && ST_MakeEnvelope(0, 0, 5, 5);
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT 'clone: seqscan', count(*) FROM gis_m_gist WHERE g && ST_MakeEnvelope(0, 0, 5, 5);
\c :db
UPDATE gis_geoms SET g = ST_Translate(g, 0, 10) WHERE id IN (1, 39);
UPDATE gis_rasters SET r = ST_SetValue(r, 1, 1, 1, 42) WHERE id = 7;
SELECT 'commit', (dolt_commit('-Am', 'gis remote change'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'push again', dolt_push('origin', 'main') IS NOT NULL;
:digests \gset src2_
\c regress_gis_clone
SELECT 'fetch', dolt_fetch() IS NOT NULL;
SELECT 'fetched, not merged', ST_AsText(g) FROM gis_geoms WHERE id = 1;
SELECT 'pull', dolt_pull() IS NOT NULL;
SELECT 'pulled', ST_AsText(g) FROM gis_geoms WHERE id = 1;
:digests \gset clone2_
SELECT 'pull: same', :'clone2_string_agg' = :'src2_string_agg';
\c :db
DROP DATABASE regress_gis_clone;

-- PostGIS in another schema than the commit's
CREATE DATABASE regress_gis_other;
\c regress_gis_other
CREATE SCHEMA gis;
CREATE EXTENSION postgis WITH SCHEMA gis;
SELECT 'clone into another layout', dolt_clone(:'url') IS NOT NULL;
SELECT 'nothing cloned', to_regclass('public.gis_geoms') IS NULL;
\c :db
DROP DATABASE regress_gis_other;
