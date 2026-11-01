-- Copying a branch's catalog rows to another branch: every versioned
-- schema and object in it made anew there, under new object ids, every
-- reference between them rewritten, the shared ones kept.  The copies'
-- storage is empty, a sequence's value aside.
SELECT pg_branch_id_allocate() AS b1 \gset
SELECT pg_branch_id_allocate() AS b2 \gset
SELECT pg_branch_id_allocate() AS b3 \gset
SELECT pg_branch_id_allocate() AS b4 \gset
CREATE SCHEMA pub;
CREATE TABLE pub.p (id int PRIMARY KEY);
INSERT INTO pub.p VALUES (1), (2);

SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id serial PRIMARY KEY, v text DEFAULT 'none' NOT NULL,
					n int CHECK (n > 0), p int REFERENCES pub.p, big text);
CREATE INDEX t_lower_v ON app.t (lower(v)) WHERE n > 1;
CREATE TABLE app.c (id int PRIMARY KEY, t int REFERENCES app.t ON DELETE CASCADE);
CREATE SEQUENCE app.s START 100;
COMMENT ON TABLE app.t IS 'the copied table';
COMMENT ON COLUMN app.t.v IS 'a column';
INSERT INTO app.t (v, n, p) SELECT 'v' || g, g, 1 FROM generate_series(1, 5) g;
SELECT nextval('app.s'), nextval('app.s');
ANALYZE app.t;
SELECT 'app.t'::regclass::oid AS t1 \gset
-- (this session's temporary view of b1's table is its own: not copied)
CREATE TEMP VIEW tv AS SELECT id FROM app.t;
CREATE FUNCTION pg_temp.try(q text) RETURNS text LANGUAGE plpgsql AS
$$BEGIN EXECUTE q; RETURN 'ok';
EXCEPTION WHEN OTHERS THEN RETURN regexp_replace(SQLERRM, '[0-9]+', 'N', 'g'); END$$;

-- copy b1 to b2, from a session on no branch
RESET catalog_branch;
SELECT pg_branch_catalog_copy(:b1, :b2);
SELECT count(*) AS copied_temp_views FROM pg_class WHERE relname = 'tv';

-- b2 has the same definitions under its own object ids, and empty tables
SET catalog_branch = :b2;
SELECT 'app.t'::regclass::oid <> :t1 AS own_oid;
SELECT c.relname, c.relkind FROM pg_class c
WHERE c.relnamespace = 'app'::regnamespace ORDER BY 1;
SELECT pg_get_constraintdef(oid) FROM pg_constraint
WHERE conrelid IN ('app.t'::regclass, 'app.c'::regclass) ORDER BY 1;
SELECT pg_get_indexdef('app.t_lower_v'::regclass);
SELECT adnum, pg_get_expr(adbin, adrelid) FROM pg_attrdef WHERE adrelid = 'app.t'::regclass ORDER BY 1;
SELECT obj_description('app.t'::regclass, 'pg_class'), col_description('app.t'::regclass, 2);
SELECT count(*) > 0 AS statistics FROM pg_statistic WHERE starelid = 'app.t'::regclass;
SELECT count(*) FROM app.t;
-- the sequence's value came with it; the serial's default is b2's sequence
SELECT nextval('app.s');
INSERT INTO app.t (n, p) VALUES (7, 2) RETURNING id, v, n;
INSERT INTO app.t (n) VALUES (0);
INSERT INTO app.t (n, p) VALUES (1, 99);
INSERT INTO app.c VALUES (1, 6);
INSERT INTO app.c VALUES (2, 1);
-- a shared row b2's copy references can't go: its foreign key checks it
DELETE FROM pub.p WHERE id = 2;
DELETE FROM app.t;
SELECT count(*) FROM app.c;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM app.t WHERE lower(v) = 'x' AND n > 1;
RESET enable_seqscan;

-- b1 is as it was
SET catalog_branch = :b1;
SELECT count(*), sum(n) FROM app.t;
SELECT nextval('app.s');

-- a branch with an object the copy doesn't know is refused, and nothing made
SET catalog_branch = :b3;
CREATE SCHEMA app VERSIONED CREATE TABLE t (x int) CREATE VIEW v AS SELECT x FROM t;
RESET catalog_branch;
SELECT pg_branch_catalog_copy(:b3, :b4);
SET catalog_branch_filter = off;
SELECT count(*) AS b4_schemas FROM pg_namespace WHERE nspbranch = :b4;
RESET catalog_branch_filter;
-- nor onto a branch that has schemas, or from a branch to itself
SELECT pg_temp.try(format('SELECT pg_branch_catalog_copy(%s, %s)', :b1, :b2));
SELECT pg_temp.try(format('SELECT pg_branch_catalog_copy(%s, %s)', :b1, :b1));

SET client_min_messages = warning;
SELECT pg_branch_catalog_drop(:b1), pg_branch_catalog_drop(:b2), pg_branch_catalog_drop(:b3);
DROP SCHEMA pub CASCADE;
