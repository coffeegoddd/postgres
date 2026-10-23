--
-- PostGIS values and history: the same table built different ways has one
-- root hash; rewrites that change only how a value is stored show no
-- change; merges combine disjoint spatial edits and conflict on different
-- bytes (spatially equal or not); geometry keys order, merge and refuse
-- as they should.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

SELECT 'on main', active_branch() = 'main';
SELECT (dolt_hashof('HEAD')) AS base \gset

-- One table, seven ways.  The values: the corpus as main has it, a NaN
-- point (written as an empty point's bytes), a large polygon.
CREATE FUNCTION gis_check.hi_rows() RETURNS TABLE (id int, g geometry) LANGUAGE sql AS $$
  SELECT id, g FROM gis_geoms WHERE g IS NOT NULL
  UNION ALL SELECT 100, ST_MakePoint('NaN', 'NaN')
  UNION ALL SELECT 101, ST_Buffer('POINT(3 3)'::geometry, 50, 9000)
$$;
CREATE FUNCTION gis_check.hi_build(branch text, how text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
  PERFORM dolt_branch(branch, current_setting('gis.base'));
  PERFORM dolt_checkout(branch);
  CREATE TABLE gis_hi (id int PRIMARY KEY, g geometry);
  IF how = 'ascending' THEN
    INSERT INTO gis_hi SELECT * FROM gis_check.hi_rows() ORDER BY id;
  ELSIF how = 'descending' THEN
    INSERT INTO gis_hi SELECT * FROM gis_check.hi_rows() ORDER BY id DESC;
  ELSIF how = 'updated' THEN
    INSERT INTO gis_hi SELECT id, 'POINT(0 0)' FROM gis_check.hi_rows();
    UPDATE gis_hi h SET g = r.g FROM gis_check.hi_rows() r WHERE r.id = h.id;
  ELSIF how = 'reinserted' THEN
    INSERT INTO gis_hi SELECT * FROM gis_check.hi_rows();
    DELETE FROM gis_hi WHERE id % 3 = 0;
    INSERT INTO gis_hi SELECT * FROM gis_check.hi_rows() WHERE id % 3 = 0;
  ELSIF how = 'stored uncompressed' THEN
    -- (out of line and uncompressed while written; the column's storage
    -- back to its default before the commit)
    ALTER TABLE gis_hi ALTER COLUMN g SET STORAGE EXTERNAL;
    INSERT INTO gis_hi SELECT * FROM gis_check.hi_rows();
    ALTER TABLE gis_hi ALTER COLUMN g SET STORAGE MAIN;
  ELSIF how = 'empty point' THEN
    INSERT INTO gis_hi SELECT id, CASE WHEN id = 100 THEN 'POINT EMPTY'::geometry ELSE g END
    FROM gis_check.hi_rows();
  ELSIF how = 'negative NaN point' THEN
    -- (other NaN bits: canonical bytes are taken as a round trip)
    INSERT INTO gis_hi SELECT id, CASE WHEN id = 100 THEN ST_MakePoint(-'NaN'::float8, -'NaN'::float8)
                                  ELSE g END
    FROM gis_check.hi_rows();
  END IF;
  PERFORM dolt_commit('-Am', how);
  PERFORM dolt_checkout('main');
  RETURN prolly_root_hash(branch);
END $$;
SELECT set_config('gis.base', :'base', false) IS NOT NULL AS set_base \gset
SELECT 'one root for one table',
  count(DISTINCT gis_check.hi_build('gis_hi_' || n, how)) = 1, count(*)
FROM unnest(ARRAY['ascending', 'descending', 'updated', 'reinserted', 'stored uncompressed',
                  'empty point', 'negative NaN point']) WITH ORDINALITY h(how, n);

-- Rewrites that only change how values are stored are no change.
SELECT 'checkout', (dolt_checkout('gis_hi_1'))[1] = '0';
UPDATE gis_hi SET g = g;
SELECT 'rewritten: no change', count(*) = 0 FROM dolt_status;
ALTER TABLE gis_hi ALTER COLUMN g SET STORAGE EXTERNAL;
UPDATE gis_hi SET g = ST_GeomFromEWKB(ST_AsEWKB(g)) WHERE id IN (38, 39, 101);
ALTER TABLE gis_hi ALTER COLUMN g SET STORAGE MAIN;
SELECT 'stored uncompressed: no change', count(*) = 0 FROM dolt_status;
SELECT 'same root after', (dolt_commit('-Am', 'restored', '--allow-empty'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'root unchanged', prolly_root_hash('HEAD') = prolly_root_hash('HEAD~1');
UPDATE gis_hi SET g = ST_Translate(g, 0, 1) WHERE id IN (1, 39, 101);
SELECT 'three moved', (dolt_commit('-Am', 'three moved'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'diff: exactly the moved rows', string_agg(diff_type || ':' || (to_row).id, ',' ORDER BY (to_row).id)
FROM dolt_diff('HEAD~1', 'HEAD', NULL::gis_hi);
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';

-- Merges.
CREATE TABLE gis_mg (id int PRIMARY KEY, g geometry);
INSERT INTO gis_mg SELECT i, ST_MakePoint(i, i) FROM generate_series(1, 20) i;
SELECT 'mg base', (dolt_commit('-Am', 'mg base'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'branch', (dolt_branch('gis_mg_side'))[1] = '0';
UPDATE gis_mg SET g = ST_Translate(g, 100, 0) WHERE id BETWEEN 11 AND 15;
UPDATE gis_mg SET g = ST_Buffer('POINT(0 0)'::geometry, 10, 4000) WHERE id = 7;
UPDATE gis_mg SET g = 'POINT(-1 -1)' WHERE id = 8;
UPDATE gis_mg SET g = 'POLYGON((0 0, 1 0, 1 1, 0 0))' WHERE id = 9;
SELECT 'mg main', (dolt_commit('-Am', 'mg main'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'checkout side', (dolt_checkout('gis_mg_side'))[1] = '0';
UPDATE gis_mg SET g = ST_Translate(g, 0, 100) WHERE id BETWEEN 1 AND 5;
-- (the same polygon, made another way)
UPDATE gis_mg SET g = ST_GeomFromEWKB(ST_AsEWKB(ST_Buffer('POINT(0 0)'::geometry, 10, 4000)))
WHERE id = 7;
UPDATE gis_mg SET g = 'POINT(-2 -2)' WHERE id = 8;
-- (the same triangle from another start point: equal, but other bytes)
UPDATE gis_mg SET g = 'POLYGON((1 0, 1 1, 0 0, 1 0))' WHERE id = 9;
SELECT 'mg side', (dolt_commit('-Am', 'mg side'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'spatially equal', ST_Equals('POLYGON((0 0, 1 0, 1 1, 0 0))', 'POLYGON((1 0, 1 1, 0 0, 1 0))');
BEGIN;
SELECT 'merge', (dolt_merge('main'))[2:4];
SELECT 'conflicts', string_agg((base_row).id || ':' || ST_AsText((our_row).g) || '|' || ST_AsText((their_row).g),
                               ', ' ORDER BY (base_row).id)
FROM dolt_conflicts(NULL::gis_mg);
SELECT 'resolve theirs', dolt_conflicts_resolve('--theirs', 'gis_mg') IS NOT NULL;
SELECT 'merged', (dolt_commit('-Am', 'mg merged'))[1] ~ '^[0-9a-f]{40}$';
COMMIT;
SELECT 'disjoint edits both kept', string_agg(id || ':' || ST_AsText(g), ',' ORDER BY id)
FROM gis_mg WHERE id IN (1, 5, 6, 11, 15, 16);
SELECT 'same polygon kept', ST_NPoints(g), ST_Equals(g, ST_Buffer('POINT(0 0)'::geometry, 10, 4000))
FROM gis_mg WHERE id = 7;
SELECT 'conflicts resolved', string_agg(id || ':' || ST_AsText(g), ',' ORDER BY id)
FROM gis_mg WHERE id IN (8, 9);
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';

-- Geometry keys: btree order as on heap, through a merge; uniqueness and
-- exclusion checked when branches meet; keys too long for a commit
-- refused where they're written.
CREATE TABLE gis_pk (g geometry PRIMARY KEY, v int);
CREATE TABLE gis_rk (region int, g geometry, v int, PRIMARY KEY (region, g));
CREATE TABLE gis_uq (id int PRIMARY KEY, g geometry UNIQUE);
CREATE TABLE gis_ex (id int PRIMARY KEY, g geometry, EXCLUDE USING gist (g WITH &&));
INSERT INTO gis_pk SELECT g, id FROM gis_idx WHERE id % 5 = 0 AND ST_NDims(g) = 2 ON CONFLICT DO NOTHING;
INSERT INTO gis_rk SELECT id % 3, g, id FROM gis_idx WHERE id % 7 = 0 AND g IS NOT NULL
ON CONFLICT DO NOTHING;
SELECT 'keys', (dolt_commit('-Am', 'keys'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'branch', (dolt_branch('gis_keys'))[1] = '0';
INSERT INTO gis_pk SELECT g, id FROM gis_idx WHERE id % 5 = 1 AND ST_NDims(g) = 2 ON CONFLICT DO NOTHING;
UPDATE gis_rk SET v = -v WHERE region = 0;
SELECT 'keys main', (dolt_commit('-Am', 'keys main'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'checkout keys', (dolt_checkout('gis_keys'))[1] = '0';
INSERT INTO gis_pk SELECT g, id FROM gis_idx WHERE id % 5 = 2 AND ST_NDims(g) = 2 ON CONFLICT DO NOTHING;
UPDATE gis_rk SET v = v + 1000 WHERE region = 1;
SELECT 'keys side', (dolt_commit('-Am', 'keys side'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'merge keys', (dolt_merge('main'))[2:4];
CREATE TABLE gis_check.pk_heap AS SELECT * FROM gis_pk;
SELECT 'heap copy', relam = (SELECT oid FROM pg_am WHERE amname = 'heap') FROM pg_class
WHERE oid = 'gis_check.pk_heap'::regclass;
SELECT 'key order as on heap',
  (SELECT array_agg(v ORDER BY g) FROM gis_pk) = (SELECT array_agg(v ORDER BY g) FROM gis_check.pk_heap),
  (SELECT count(*) FROM gis_pk);
SELECT 'composite keys merged', count(*) FILTER (WHERE v < 0), count(*) FILTER (WHERE v > 1000)
FROM gis_rk;
SELECT 'merged keys', (dolt_commit('-Am', 'keys merged', '--allow-empty'))[1] ~ '^[0-9a-f]{40}$';
INSERT INTO gis_uq VALUES (2, 'POINT(5 5)');
INSERT INTO gis_ex VALUES (2, 'POLYGON((1 0, 3 0, 3 2, 1 0))');
SELECT 'side violations', (dolt_commit('-Am', 'side violations'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';
INSERT INTO gis_uq VALUES (1, 'POINT(5 5)');
INSERT INTO gis_ex VALUES (1, 'POLYGON((0 0, 2 0, 2 2, 0 0))');
SELECT 'main violations', (dolt_commit('-Am', 'main violations'))[1] ~ '^[0-9a-f]{40}$';
SELECT 'checkout keys', (dolt_checkout('gis_keys'))[1] = '0';
BEGIN;
SELECT 'merge violating', (dolt_merge('main'))[2:4];
SELECT 'violations', string_agg(table_name || ' ' || num_violations, ', ' ORDER BY table_name)
FROM dolt_constraint_violations();
SELECT 'unique violation', string_agg(violation_type, ',') FROM dolt_constraint_violations('gis_uq');
SELECT 'exclusion violation', string_agg(violation_type, ',') FROM dolt_constraint_violations('gis_ex');
ROLLBACK;
SELECT 'nothing merged', (SELECT string_agg(id::text, ',') FROM gis_uq), (SELECT string_agg(id::text, ',') FROM gis_ex);
SELECT 'long key', gis_check.outcome(
  $$INSERT INTO gis_pk VALUES (ST_Buffer('POINT(0 0)'::geometry, 10, 2000), 0)$$);
SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';
