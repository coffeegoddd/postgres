-- The catalog filter: a session's queries of the catalogs see its
-- branch's rows and the shared ones, not other branches'; lookups by
-- object id see every branch's.
SELECT pg_branch_id_allocate() AS b1 \gset
SELECT pg_branch_id_allocate() AS b2 \gset

CREATE SCHEMA pub;
CREATE TABLE pub.p (id int PRIMARY KEY, note text);
INSERT INTO pub.p VALUES (1, 'shared');

-- one versioned schema, defined on each branch with a few kinds of object
SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED;
CREATE TYPE app.mood AS ENUM ('ok', 'sad');
CREATE TABLE app.t (id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
					v int NOT NULL, m app.mood NOT NULL DEFAULT 'ok',
					p int REFERENCES pub.p, big text);
CREATE INDEX t_v ON app.t (v);
CREATE VIEW app.tv AS SELECT id, v FROM app.t WHERE v > 10;
CREATE FUNCTION app.f(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 2';
COMMENT ON TABLE app.t IS 'branch one';
INSERT INTO app.t (v, p) SELECT g, 1 FROM generate_series(1, 100) g;
ANALYZE app.t;
SELECT 'app.t'::regclass::oid AS t1 \gset

SET catalog_branch = :b2;
CREATE SCHEMA app VERSIONED;
CREATE TYPE app.mood AS ENUM ('ok', 'sad', 'glad');
CREATE TABLE app.t (id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
					v int NOT NULL, m app.mood NOT NULL DEFAULT 'ok',
					p int REFERENCES pub.p, big text, extra int DEFAULT 9);
CREATE INDEX t_m ON app.t (m, extra);
CREATE VIEW app.tv AS SELECT id, v FROM app.t WHERE v > 10;
CREATE FUNCTION app.f(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 3';
COMMENT ON TABLE app.t IS 'branch two';
INSERT INTO app.t (v, p) SELECT g, 1 FROM generate_series(1, 150) g;
SELECT 'app.t'::regclass::oid AS t2 \gset
SELECT 'app.tv'::regclass::oid AS tv2 \gset

-- each branch sees one schema app, and its own definitions in it
SET catalog_branch = :b1;
SELECT count(*) FROM pg_namespace WHERE nspname = 'app';
SELECT string_agg(column_name, ',' ORDER BY ordinal_position)
FROM information_schema.columns WHERE table_schema = 'app' AND table_name = 't';
SELECT enum_range(NULL::app.mood);
SELECT string_agg(indexname, ',' ORDER BY indexname) FROM pg_indexes
WHERE schemaname = 'app';
SELECT app.f(5), obj_description('app.t'::regclass, 'pg_class');
SELECT relname, relkind FROM pg_class
WHERE relnamespace = 'app'::regnamespace ORDER BY relname;
\d app.t

SET catalog_branch = :b2;
SELECT count(*) FROM pg_namespace WHERE nspname = 'app';
SELECT string_agg(column_name, ',' ORDER BY ordinal_position)
FROM information_schema.columns WHERE table_schema = 'app' AND table_name = 't';
SELECT enum_range(NULL::app.mood);
SELECT string_agg(indexname, ',' ORDER BY indexname) FROM pg_indexes
WHERE schemaname = 'app';
SELECT app.f(5), obj_description('app.t'::regclass, 'pg_class');
\d app.t

-- every filtered catalog, by what its rows are or describe: one branch's
-- rows, then the other's
CREATE TEMP TABLE probes (catalog text, q text);
INSERT INTO probes VALUES
	('pg_class', $$SELECT count(*) FROM pg_class WHERE relname IN ('t', 'tv', 't_pkey')$$),
	('pg_type', $$SELECT count(*) FROM pg_type WHERE typname IN ('mood', 't', '_mood')$$),
	('pg_proc', $$SELECT count(*) FROM pg_proc WHERE proname = 'f'$$),
	('pg_constraint', $$SELECT count(*) FROM pg_constraint WHERE conname IN ('t_pkey', 't_p_fkey')$$),
	('pg_attribute', $$SELECT count(*) FROM pg_attribute WHERE attname = 'big'$$),
	('pg_attrdef', $$SELECT count(*) FROM pg_attrdef d JOIN pg_attribute a ON a.attrelid = d.adrelid AND a.attnum = d.adnum WHERE a.attname = 'm'$$),
	('pg_index', $$SELECT count(*) FROM pg_index WHERE indrelid IN (SELECT oid FROM pg_class WHERE relname = 't')$$),
	('pg_enum', $$SELECT count(*) FROM pg_enum WHERE enumlabel = 'sad'$$),
	('pg_rewrite', $$SELECT count(*) FROM pg_rewrite WHERE rulename = '_RETURN' AND ev_class IN (SELECT oid FROM pg_class WHERE relname = 'tv')$$),
	('pg_sequence', $$SELECT count(*) FROM pg_sequence s JOIN pg_class c ON c.oid = s.seqrelid WHERE c.relname = 't_id_seq'$$),
	('pg_statistic', $$SELECT count(*) > 0 FROM pg_statistic s JOIN pg_class c ON c.oid = s.starelid WHERE c.relname = 't'$$),
	('pg_trigger on a shared table', $$SELECT count(*) FROM pg_trigger WHERE tgrelid = 'pub.p'::regclass$$),
	('pg_depend', $$SELECT count(*) > 0 FROM pg_depend WHERE refobjid IN (SELECT oid FROM pg_type WHERE typname = 'mood')$$),
	('pg_description', $$SELECT string_agg(description, ',') FROM pg_description WHERE description LIKE 'branch%'$$),
	('pg_class, TOAST', format($$SELECT count(*) FROM pg_class WHERE relname IN ('pg_toast_%s', 'pg_toast_%s')$$, :t1, :t2));

CREATE FUNCTION pg_temp.probe(q text) RETURNS text LANGUAGE plpgsql AS
$$DECLARE r text; BEGIN EXECUTE q INTO r; RETURN r; END$$;

SET catalog_branch = :b1;
SELECT catalog, pg_temp.probe(q) AS b1 FROM probes;
SET catalog_branch = :b2;
SELECT catalog, pg_temp.probe(q) AS b2 FROM probes;
RESET catalog_branch;
SELECT catalog, pg_temp.probe(q) AS none FROM probes;
SET catalog_branch_filter = off;
SELECT catalog, pg_temp.probe(q) AS everything FROM probes;
RESET catalog_branch_filter;

-- the filter holds whichever way a query reads a catalog
SET catalog_branch = :b1;
VACUUM pg_class;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM pg_class WHERE relname = 't';
SELECT count(*) FROM pg_class WHERE relname = 't';
SET enable_indexonlyscan = off;
SELECT count(*), min(relnatts) FROM pg_class WHERE relname = 't';
SET enable_indexscan = off;
SET enable_bitmapscan = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM pg_class WHERE relname = 't';
SELECT count(*) FROM pg_class WHERE relname = 't';
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
RESET enable_bitmapscan;
SELECT count(*) FROM pg_class TABLESAMPLE SYSTEM (100) WHERE relname = 't';
SELECT count(*) FROM pg_class TABLESAMPLE BERNOULLI (100) WHERE relname = 't';
SELECT count(*) FROM pg_class WHERE ctid = (SELECT ctid FROM pg_class WHERE oid = :t2);
SELECT count(*) FROM pg_class WHERE ctid = (SELECT ctid FROM pg_class WHERE oid = :t1);
SET debug_parallel_query = on;
SELECT count(*) FROM pg_class WHERE relname = 't';
RESET debug_parallel_query;
COPY (SELECT relname FROM pg_class WHERE relname = 't') TO stdout;
BEGIN;
DECLARE c CURSOR FOR SELECT relname FROM pg_class WHERE relname IN ('t', 'tv') ORDER BY relname;
FETCH ALL FROM c;
ROLLBACK;

-- lookups by object id see every branch's objects, but a function that
-- reads the catalogs with a query sees what queries see
SELECT :t2::regclass::text AS other_branch_table;
SELECT has_column_privilege(:t2, 'extra', 'SELECT') AS other_branch_column;
SELECT pg_get_viewdef(:tv2) IS NULL AS other_branch_view_by_query;

-- a role owning another branch's objects cannot be dropped, and the
-- error names them, from any branch
CREATE ROLE regress_branch_role;
SET catalog_branch = :b2;
ALTER TABLE app.t OWNER TO regress_branch_role;
SET catalog_branch = :b1;
DROP ROLE regress_branch_role;
RESET catalog_branch;
DROP ROLE regress_branch_role;
SET catalog_branch = :b2;
ALTER TABLE app.t OWNER TO CURRENT_USER;
DROP ROLE regress_branch_role;

-- each branch's table has its own numbers in pg_class, and VACUUM keeps
-- them so; the database's freeze horizon is the oldest over both
SET catalog_branch = :b1;
VACUUM app.t;
SELECT relpages > 0 AS pages, reltuples FROM pg_class WHERE oid = 'app.t'::regclass;
SET catalog_branch = :b2;
DELETE FROM app.t WHERE v > 120;
VACUUM app.t;
SELECT relpages > 0 AS pages, reltuples FROM pg_class WHERE oid = 'app.t'::regclass;
SET catalog_branch_filter = off;
SELECT (SELECT datfrozenxid FROM pg_database WHERE datname = current_database())::text::int8
	<= min(relfrozenxid::text::int8) AS horizon_covers_both
FROM pg_class WHERE oid IN (:t1, :t2);
RESET catalog_branch_filter;

-- a cursor's rows are the branch's it began on, though the session
-- changes branch between fetches: by a sequential scan and an index scan
CREATE FUNCTION pg_temp.cursor_rows(q text, then_branch int) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	c refcursor;
	r record;
	got text[] := '{}';
BEGIN
	OPEN c FOR EXECUTE q;
	FETCH c INTO r;
	got := got || r.x;
	PERFORM set_config('catalog_branch', then_branch::text, true);
	LOOP
		FETCH c INTO r;
		EXIT WHEN NOT FOUND;
		got := got || r.x;
	END LOOP;
	RETURN (SELECT string_agg(g, ',' ORDER BY g) FROM unnest(got) g);
END $$;
SET catalog_branch = :b1;
SELECT format('SELECT CASE oid WHEN %s THEN ''b1'' WHEN %s THEN ''b2'' ELSE relname END AS x
	FROM pg_class WHERE relname IN (''pg_class'', ''t'')', :t1, :t2) AS q \gset
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT pg_temp.cursor_rows(:'q', :b2) AS by_seqscan;
RESET enable_bitmapscan;
SET enable_seqscan = off;
SET enable_indexscan = on;
SELECT pg_temp.cursor_rows(:'q', :b2) AS by_indexscan;
RESET enable_seqscan;
RESET enable_indexscan;
SELECT current_setting('catalog_branch')::int = :b1 AS back_on_b1;
SET catalog_branch = :b2;

DROP SCHEMA app CASCADE;
SET catalog_branch = :b1;
DROP SCHEMA app CASCADE;
RESET catalog_branch;
DROP SCHEMA pub CASCADE;
