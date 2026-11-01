-- Copying a branch's catalog rows to another branch: every versioned
-- schema and object in it made anew there, under new object ids, every
-- reference between them rewritten, the shared ones kept.  The copies'
-- storage is empty, a sequence's value aside.
SELECT pg_branch_id_allocate() AS b1 \gset
SELECT pg_branch_id_allocate() AS b2 \gset
SELECT pg_branch_id_allocate() AS b3 \gset
SELECT pg_branch_id_allocate() AS b4 \gset
SELECT pg_branch_id_allocate() AS b5 \gset
SELECT pg_branch_id_allocate() AS b6 \gset
SELECT pg_branch_id_allocate() AS b7 \gset
SELECT pg_branch_id_allocate() AS b8 \gset
CREATE SCHEMA pub;
CREATE TABLE pub.p (id int PRIMARY KEY);
INSERT INTO pub.p VALUES (1), (2);
CREATE DOMAIN pub.posint AS int CHECK (VALUE > 0);

SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id serial PRIMARY KEY, v text DEFAULT 'none' NOT NULL,
					n int CHECK (n > 0), p int REFERENCES pub.p, big text);
CREATE INDEX t_lower_v ON app.t (lower(v)) WHERE n > 1;
CREATE TABLE app.c (id int PRIMARY KEY, t int REFERENCES app.t ON DELETE CASCADE,
					rels regclass[] DEFAULT '{pub.p}');
CREATE TABLE app.ident (id int GENERATED ALWAYS AS IDENTITY, x pub.posint DEFAULT 1,
						big text, CHECK ((x, id) > (0, 0)));
CREATE SEQUENCE app.s START 100;
ALTER DEFAULT PRIVILEGES IN SCHEMA app GRANT SELECT ON TABLES TO PUBLIC;
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
-- an identity column from its own sequence, a shared domain's default, a
-- toasted value in the copy's own TOAST table, named after the copy
INSERT INTO app.ident (big) SELECT string_agg(md5(g::text), '') FROM generate_series(1, 300) g
	RETURNING id, x, length(big);
SELECT t.relname = 'pg_toast_' || 'app.ident'::regclass::oid AS toast_named_after_copy
FROM pg_class c JOIN pg_class t ON t.oid = c.reltoastrelid WHERE c.oid = 'app.ident'::regclass;
SELECT count(*) AS default_privileges FROM pg_default_acl WHERE defaclnamespace = 'app'::regnamespace;
-- DDL on the copy afterwards
ALTER TABLE app.ident ADD COLUMN extra int;
DELETE FROM app.t;
SELECT count(*) FROM app.c;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM app.t WHERE lower(v) = 'x' AND n > 1;
RESET enable_seqscan;

-- b1 is as it was: its rows, its sequences, its foreign key, its triggers
SET catalog_branch = :b1;
SELECT count(*), sum(n) FROM app.t;
SELECT nextval('app.s');
SELECT last_value FROM app.t_id_seq;
DELETE FROM pub.p WHERE id = 1;
SELECT count(*) AS triggers_on_pub_p FROM pg_trigger WHERE tgrelid = 'pub.p'::regclass;

-- a copy of a copy, from b2 to b5
RESET catalog_branch;
SELECT pg_branch_catalog_copy(:b2, :b5);
SET catalog_branch = :b5;
SELECT count(*) AS tables FROM pg_class WHERE relnamespace = 'app'::regnamespace AND relkind = 'r';
SELECT nextval('app.s');

-- a copy rolled back leaves nothing
RESET catalog_branch;
BEGIN;
SELECT pg_branch_catalog_copy(:b1, :b6);
ROLLBACK;
SET catalog_branch_filter = off;
SELECT count(*) AS b6_schemas FROM pg_namespace WHERE nspbranch = :b6;
RESET catalog_branch_filter;

-- every row made is checked: a constant naming a source object refuses the copy
SET catalog_branch = :b7;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.k (r oid);
DO $$BEGIN EXECUTE format('ALTER TABLE app.k ADD CHECK (r <> %L::oid)', 'app.k'::regclass::oid); END$$;
RESET catalog_branch;
SELECT pg_temp.try(format('SELECT pg_branch_catalog_copy(%s, %s)', :b7, :b8));
-- and a table holding values of the branch's own composite type
SET catalog_branch = :b7;
ALTER TABLE app.k DROP CONSTRAINT k_r_check;
CREATE TABLE app.holder (k app.k);
RESET catalog_branch;
SELECT pg_temp.try(format('SELECT pg_branch_catalog_copy(%s, %s)', :b7, :b8));

-- a branch with an object the copy doesn't know is refused, and nothing made
CREATE ACCESS METHOD heap2 TYPE TABLE HANDLER heap_tableam_handler;
SET catalog_branch = :b3;
CREATE SCHEMA app VERSIONED CREATE TABLE t (x int);
CREATE TABLE app.u (x int) USING heap2;
RESET catalog_branch;
SELECT pg_branch_catalog_copy(:b3, :b4);
SET catalog_branch_filter = off;
SELECT count(*) AS b4_schemas FROM pg_namespace WHERE nspbranch = :b4;
RESET catalog_branch_filter;
-- nor onto a branch that has schemas, or from a branch to itself
SELECT pg_temp.try(format('SELECT pg_branch_catalog_copy(%s, %s)', :b1, :b2));
SELECT pg_temp.try(format('SELECT pg_branch_catalog_copy(%s, %s)', :b1, :b1));

-- functions, triggers, policies, views, rules, and types of the branch's own
SELECT pg_branch_id_allocate() AS b9 \gset
SELECT pg_branch_id_allocate() AS b10 \gset
CREATE ROLE regress_copy_reader;
SET catalog_branch = :b9;
CREATE SCHEMA app VERSIONED;
CREATE TYPE app.mood AS ENUM ('sad', 'ok', 'glad');
CREATE TYPE app.pair AS (a int, b int);
CREATE TYPE app.span AS RANGE (subtype = int);
CREATE DOMAIN app.small AS int CHECK (VALUE < 100) DEFAULT 7;
CREATE TABLE app.items (id int PRIMARY KEY, owner text, n app.small, cnt int DEFAULT 0);
CREATE TABLE app.log (item int, what text);
CREATE FUNCTION app.double(x int) RETURNS int LANGUAGE sql IMMUTABLE RETURN x * 2;
CREATE FUNCTION app.pair_sum(p app.pair) RETURNS int LANGUAGE sql RETURN p.a + p.b;
CREATE FUNCTION app.happier(m app.mood DEFAULT 'sad') RETURNS bool LANGUAGE sql RETURN m > 'ok'::app.mood;
CREATE FUNCTION app.in_span(x int) RETURNS bool LANGUAGE sql RETURN x <@ app.span(1, 10);
CREATE FUNCTION app.logit() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN INSERT INTO app.log VALUES (NEW.id, TG_OP); RETURN NEW; END $$;
CREATE TRIGGER items_log AFTER INSERT ON app.items FOR EACH ROW EXECUTE FUNCTION app.logit();
CREATE VIEW app.doubled AS SELECT id, app.double(n) AS d FROM app.items WHERE n > 1;
CREATE VIEW app.glad AS SELECT 'glad'::app.mood > 'ok'::app.mood AS gladder;
CREATE VIEW app.by_owner AS SELECT id, owner FROM app.items GROUP BY id;
CREATE MATERIALIZED VIEW app.counted AS SELECT count(*) AS c FROM app.items;
CREATE RULE items_count AS ON UPDATE TO app.items DO ALSO
	INSERT INTO app.log VALUES (NEW.id, 'updated');
ALTER TABLE app.items ENABLE ROW LEVEL SECURITY;
CREATE POLICY own ON app.items FOR SELECT USING (owner = current_user);
GRANT USAGE ON SCHEMA app TO regress_copy_reader;
GRANT SELECT ON app.items TO regress_copy_reader;
INSERT INTO app.items (id, owner, n) VALUES (1, 'regress_copy_reader', 5);
RESET catalog_branch;
SELECT pg_branch_catalog_copy(:b9, :b10);

SET catalog_branch = :b10;
SELECT proname FROM pg_proc WHERE pronamespace = 'app'::regnamespace ORDER BY 1;
SELECT app.double(21), app.pair_sum(ROW(1, 2)::app.pair), app.happier('glad'), app.in_span(5);
SELECT * FROM app.glad;
SELECT app.happier();
-- the trigger, the rule, the view and the domain act on b10's own objects
INSERT INTO app.items (id, owner) VALUES (2, 'regress_copy_reader') RETURNING n;
INSERT INTO app.items (id, owner, n) VALUES (3, 'x', 500);
UPDATE app.items SET cnt = 1 WHERE id = 2;
SELECT * FROM app.log ORDER BY 1, 2;
SELECT * FROM app.doubled ORDER BY 1;
REFRESH MATERIALIZED VIEW app.counted;
SELECT * FROM app.counted;
-- the policy: another role sees only its own rows
INSERT INTO app.items (id, owner, n) VALUES (4, 'someone else', 2);
SET ROLE regress_copy_reader;
SELECT id FROM app.items ORDER BY 1;
RESET ROLE;
-- b9's are as they were
SET catalog_branch = :b9;
SELECT count(*) AS b9_log FROM app.log;
SELECT count(*) AS b9_items FROM app.items;

-- a table storing values of the branch's own enum, or range, is refused:
-- its data, which the branches share, names their object ids
SELECT pg_branch_id_allocate() AS b11 \gset
SET catalog_branch = :b9;
CREATE TABLE app.moods (m app.mood);
RESET catalog_branch;
SELECT pg_temp.try(format('SELECT pg_branch_catalog_copy(%s, %s)', :b9, :b11));
SET catalog_branch = :b9;
DROP TABLE app.moods;
CREATE TABLE app.spans (s app.span[]);
RESET catalog_branch;
SELECT pg_temp.try(format('SELECT pg_branch_catalog_copy(%s, %s)', :b9, :b11));
-- and an array of the branch's own domain (its header names the domain),
-- and an index holding the branch's own enum
SET catalog_branch = :b9;
DROP TABLE app.spans;
CREATE TABLE app.smalls (xs app.small[]);
RESET catalog_branch;
SELECT pg_temp.try(format('SELECT pg_branch_catalog_copy(%s, %s)', :b9, :b11));
SET catalog_branch = :b9;
DROP TABLE app.smalls;
CREATE INDEX items_mood ON app.items ((CASE WHEN n > 1 THEN 'glad'::app.mood ELSE 'sad'::app.mood END));
RESET catalog_branch;
SELECT pg_temp.try(format('SELECT pg_branch_catalog_copy(%s, %s)', :b9, :b11));

SET client_min_messages = warning;
RESET catalog_branch;
SELECT pg_branch_catalog_drop(:b9), pg_branch_catalog_drop(:b10);
DROP ROLE regress_copy_reader;
RESET client_min_messages;

-- partitions, a foreign key into a shared partitioned table, aggregates,
-- extended statistics, an extension of the branch's own, and membership
-- of a shared publication
SELECT pg_branch_id_allocate() AS b12 \gset
SELECT pg_branch_id_allocate() AS b13 \gset
CREATE TABLE pub.pp (id int PRIMARY KEY) PARTITION BY RANGE (id);
CREATE TABLE pub.pp1 PARTITION OF pub.pp FOR VALUES FROM (0) TO (100);
INSERT INTO pub.pp VALUES (1), (2);
SET client_min_messages = warning;
CREATE PUBLICATION branch_copy_pub;
RESET client_min_messages;
SET catalog_branch = :b12;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.m (k int, v int, r int REFERENCES pub.pp) PARTITION BY LIST (k);
CREATE TABLE app.m1 PARTITION OF app.m FOR VALUES IN (1);
CREATE TABLE app.mdef PARTITION OF app.m DEFAULT;
CREATE INDEX m_v ON app.m (v);
CREATE AGGREGATE app.total(int) (sfunc = int4pl, stype = int, initcond = '0');
CREATE STATISTICS app.m1_stats (dependencies) ON k, v FROM app.m1;
CREATE EXTENSION pg_trgm SCHEMA app;
SET client_min_messages = warning;
ALTER PUBLICATION branch_copy_pub ADD TABLE app.m1;
RESET client_min_messages;
INSERT INTO app.m VALUES (1, 10, 1), (2, 20, 2);
RESET catalog_branch;
SELECT pg_branch_catalog_copy(:b12, :b13);

SET catalog_branch = :b13;
SELECT c.relname, c.relkind, c.relispartition, pg_get_expr(c.relpartbound, c.oid) AS bound
FROM pg_class c WHERE c.relnamespace = 'app'::regnamespace AND c.relkind IN ('p', 'r', 'I', 'i')
	AND c.relname LIKE 'm%' ORDER BY 1;
SELECT inhrelid::regclass, inhparent::regclass FROM pg_inherits
WHERE inhparent IN ('app.m'::regclass, 'app.m_v'::regclass) ORDER BY 1;
SELECT indexrelid::regclass, indrelid::regclass FROM pg_index
WHERE indexrelid = 'app.m_v'::regclass;
INSERT INTO app.m VALUES (1, 11, 1), (3, 30, 2) RETURNING tableoid::regclass, k;
INSERT INTO app.m VALUES (1, 12, 99);
-- the foreign key into the shared partitioned table holds for the copy
DELETE FROM pub.pp WHERE id = 2;
SELECT app.total(v) FROM app.m;
SELECT stxname FROM pg_statistic_ext WHERE stxrelid = 'app.m1'::regclass;
SELECT extname, extnamespace::regnamespace FROM pg_extension WHERE extname = 'pg_trgm';
SELECT round(app.similarity('abc', 'abd')::numeric, 2);
SELECT count(*) AS published FROM pg_publication_tables
WHERE pubname = 'branch_copy_pub' AND schemaname = 'app';
-- b12's are as they were
SET catalog_branch = :b12;
SELECT count(*) FROM app.m;
SET catalog_branch_filter = off;
SELECT count(*) AS publication_members FROM pg_publication_rel r
	JOIN pg_publication p ON p.oid = r.prpubid WHERE p.pubname = 'branch_copy_pub';
RESET catalog_branch_filter;

SET client_min_messages = warning;
RESET catalog_branch;
SELECT pg_branch_catalog_drop(:b12), pg_branch_catalog_drop(:b13);
DROP PUBLICATION branch_copy_pub;
DROP TABLE pub.pp;
RESET client_min_messages;

SET client_min_messages = warning;
SELECT pg_branch_catalog_drop(:b1), pg_branch_catalog_drop(:b2), pg_branch_catalog_drop(:b3),
	   pg_branch_catalog_drop(:b5), pg_branch_catalog_drop(:b7);
DROP ACCESS METHOD heap2 CASCADE;
DROP SCHEMA pub CASCADE;
