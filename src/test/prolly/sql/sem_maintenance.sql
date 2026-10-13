--
-- Semantics: maintenance and table organization.  VACUUM, ANALYZE, CLUSTER,
-- partitioning, inheritance and materialized views behave exactly as on
-- heap (results only; sizes and plans are the engine's own).
--
CREATE TABLE maint (id int PRIMARY KEY, k int, t text);
INSERT INTO maint SELECT g, g % 10, md5(g::text) FROM generate_series(1, 10000) g;
DELETE FROM maint WHERE id % 2 = 0;
UPDATE maint SET k = k + 100 WHERE id % 5 = 0;

VACUUM maint;
VACUUM (ANALYZE) maint;
SELECT count(*), sum(id), sum(k) FROM maint;
SELECT reltuples > 0 AS analyzed FROM pg_class WHERE relname = 'maint';
SELECT count(*) > 0 AS has_stats FROM pg_stats WHERE tablename = 'maint';

VACUUM FULL maint;
SELECT count(*), sum(id), sum(k) FROM maint;
CLUSTER maint USING maint_pkey;
SELECT count(*), sum(id), sum(k) FROM maint;
SELECT id FROM maint ORDER BY id LIMIT 3;

-- Partitioned tables: partitions use the engine; rows move between them.
CREATE TABLE part (id int, region text) PARTITION BY LIST (region);
CREATE TABLE part_east PARTITION OF part FOR VALUES IN ('east');
CREATE TABLE part_west PARTITION OF part FOR VALUES IN ('west');
CREATE TABLE part_rest PARTITION OF part DEFAULT;
INSERT INTO part SELECT g, (ARRAY['east', 'west', 'north'])[g % 3 + 1]
FROM generate_series(1, 300) g;
UPDATE part SET region = 'west' WHERE id <= 30;
SELECT tableoid::regclass AS partition, count(*) FROM part GROUP BY 1 ORDER BY 1;
SELECT c.relname, a.amname = current_setting('default_table_access_method') AS uses_engine
FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relname LIKE 'part\_%' ORDER BY 1;

-- Inheritance.
CREATE TABLE base (id int, t text);
CREATE TABLE derived (extra int) INHERITS (base);
INSERT INTO base VALUES (1, 'base');
INSERT INTO derived VALUES (2, 'derived', 42);
SELECT * FROM base ORDER BY id;
SELECT * FROM ONLY base ORDER BY id;

-- Materialized views.
CREATE MATERIALIZED VIEW maint_summary AS
	SELECT k, count(*) AS n FROM maint GROUP BY k;
CREATE UNIQUE INDEX ON maint_summary (k);
SELECT * FROM maint_summary ORDER BY k LIMIT 3;
INSERT INTO maint VALUES (20001, 1, 'new');
REFRESH MATERIALIZED VIEW CONCURRENTLY maint_summary;
SELECT * FROM maint_summary WHERE k = 1;

DROP MATERIALIZED VIEW maint_summary;
DROP TABLE maint, part, base, derived CASCADE;
