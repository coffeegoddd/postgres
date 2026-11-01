-- A versioned schema's relations are block-mapped, so that branches can
-- share their pages; every other relation stays on md.c.  A TOAST table
-- and an index follow their table, and a relation keeps its storage
-- manager through a rewrite.
SELECT pg_branch_id_allocate() AS b1 \gset
CREATE FUNCTION pg_temp.block_mapped(r regclass) RETURNS bool LANGUAGE sql
	AS $$SELECT (pg_relation_shared_pages(r)).pages IS NOT NULL$$;

CREATE SCHEMA pub;
CREATE TABLE pub.t (id int PRIMARY KEY, big text);
CREATE SEQUENCE pub.s;

SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, big text);
CREATE INDEX t_big ON app.t (big);
CREATE SEQUENCE app.s;
CREATE MATERIALIZED VIEW app.mv AS SELECT 1 AS x;
CREATE TABLE app.parted (a int) PARTITION BY LIST (a);
CREATE TABLE app.part1 PARTITION OF app.parted FOR VALUES IN (1);
-- a versioned schema's relations are logged: an unlogged one is refused
CREATE UNLOGGED TABLE app.u (x int);
CREATE UNLOGGED TABLE app.u2 AS SELECT 1 AS x;
CREATE UNLOGGED SEQUENCE app.us;
CREATE TABLE app.l (x int);
ALTER TABLE app.l SET UNLOGGED;
ALTER SEQUENCE app.s SET UNLOGGED;
DROP TABLE app.l;
CREATE TEMP TABLE tmp (x int);
INSERT INTO app.t SELECT g, repeat(md5(g::text), 200) FROM generate_series(1, 50) g;
CREATE TABLE app.ctas AS SELECT 1 AS x;
CREATE TABLE app.ident (id int GENERATED ALWAYS AS IDENTITY, n int);
CREATE INDEX parted_a ON app.parted (a);
CREATE INDEX CONCURRENTLY t_id_big ON app.t (id, big);
-- (a TOAST table made later follows its table)
ALTER TABLE app.ident ADD COLUMN note text;

CREATE TEMP VIEW storage AS
SELECT n.nspname || '.' || c.relname AS relation, c.relkind,
	   pg_temp.block_mapped(c.oid) AS block_mapped
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname IN ('app', 'pub') AND c.relkind NOT IN ('p', 'I')
UNION ALL
SELECT 'toast of ' || c.relname || ' (' || n.nspname || ')', 't',
	   pg_temp.block_mapped(c.reltoastrelid)
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname IN ('app', 'pub') AND c.relname IN ('t', 'ident')
UNION ALL
SELECT 'toast index of ' || c.relname || ' (' || n.nspname || ')', 'i',
	   pg_temp.block_mapped(i.indexrelid)
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
	 JOIN pg_index i ON i.indrelid = c.reltoastrelid
WHERE n.nspname IN ('app', 'pub') AND c.relname IN ('t', 'ident')
UNION ALL
SELECT 'pg_temp.tmp', 'r', pg_temp.block_mapped('pg_temp.tmp');
SELECT * FROM storage ORDER BY relation;

-- a rewrite, and new storage for the same relation, keep it
VACUUM FULL app.t;
CLUSTER app.t USING t_pkey;
ALTER TABLE app.t ALTER COLUMN id TYPE bigint;
TRUNCATE app.part1;
REFRESH MATERIALIZED VIEW app.mv;
REINDEX TABLE app.t;
REINDEX INDEX CONCURRENTLY app.t_id_big;
ALTER TABLE app.ctas SET ACCESS METHOD heap;
SELECT * FROM storage WHERE relation LIKE '%app%' ORDER BY relation;
SELECT count(*), sum(length(big)) FROM app.t;

DROP SCHEMA app CASCADE;
RESET catalog_branch;
DROP SCHEMA pub CASCADE;
