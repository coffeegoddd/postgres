-- Revision-qualified table names, as Doltgres's: "db/ref".schema.table
-- reads the table as a branch's working set (uncommitted changes
-- included), a tag or a commit has it, anywhere a query names a table.
CREATE DATABASE regress_revisions;
\c regress_revisions
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE SCHEMA rn VERSIONED;
CREATE TABLE rn.t (id int PRIMARY KEY, v int);
INSERT INTO rn.t SELECT g, 1 FROM generate_series(1, 10) g;
CREATE TABLE public.plain (n int);
INSERT INTO public.plain VALUES (42);
SELECT length(dolt_commit('-Am', 'first')::text) AS committed;
SELECT dolt_hashof('HEAD') AS c1 \gset
SELECT dolt_branch('b1');
SELECT dolt_checkout('b1') IS NOT NULL AS on_b1;
UPDATE rn.t SET v = 2 WHERE id = 1;		-- uncommitted on b1
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- the issue's probe: b1's working set holds 11, its head 10
SELECT sum(v) FROM dolt_at('b1', NULL::rn.t);
SELECT sum(v) FROM "regress_revisions/b1".rn.t;
-- the branch's name in any case, as Dolt matches it
SELECT sum(v) FROM "regress_revisions/B1".rn.t;
-- in joins with the session's tables, aggregates and subqueries
SELECT m.id, m.v AS main_v, b.v AS b1_v
  FROM rn.t m JOIN "regress_revisions/b1".rn.t b USING (id)
 WHERE m.v <> b.v;
SELECT count(*) FROM (SELECT id FROM "regress_revisions/b1".rn.t
                      EXCEPT SELECT id FROM rn.t) d;
SELECT id, (SELECT v FROM "regress_revisions/b1".rn.t b WHERE b.id = m.id) AS b1_v
  FROM rn.t m WHERE id <= 2 ORDER BY id;
SELECT x FROM "regress_revisions/b1".rn.t x WHERE id = 1;
-- an equality or range on the copy's index is a seek in it
EXPLAIN (COSTS OFF) SELECT v FROM "regress_revisions/b1".rn.t WHERE id = 3;
EXPLAIN (COSTS OFF)
SELECT (SELECT v FROM "regress_revisions/b1".rn.t b WHERE b.id = m.id) FROM rn.t m;
SELECT v FROM "regress_revisions/b1".rn.t WHERE id = 1;
SELECT count(*) FROM "regress_revisions/b1".rn.t WHERE id BETWEEN 2 AND 4;
SELECT count(*) FROM "regress_revisions/b1".rn.t WHERE id = NULL::int;

-- the session's own branch: an ordinary table name, writable
EXPLAIN (COSTS OFF) SELECT * FROM "regress_revisions/main".rn.t;
UPDATE "regress_revisions/main".rn.t SET v = 1 WHERE id = 10;
-- a table outside the versioned schemas: every revision's
SELECT n FROM "regress_revisions/b1".public.plain;

-- commits, tags, and ancestry: read from the commit's tree
UPDATE rn.t SET v = 3 WHERE id = 2;
SELECT length(dolt_commit('-am', 'second')::text) AS committed;
SELECT dolt_tag('tag1', 'HEAD') IS NOT NULL AS tagged;
SELECT sum(v) FROM "regress_revisions/tag1".rn.t;
SELECT sum(v) FROM "regress_revisions/TAG1".rn.t;
SELECT sum(v) FROM "regress_revisions/tag1~1".rn.t;
SELECT sum(v) FROM "regress_revisions/main~1".rn.t;
SELECT 'regress_revisions/' || :'c1' AS at_c1 \gset
SELECT sum(v) FROM :"at_c1".rn.t;
-- a branch with only its head: its working set is its head commit
SELECT dolt_branch('h1');
SELECT EXISTS (SELECT FROM pg_branch WHERE brname = 'h1') AS h1_used;
SELECT sum(v) FROM "regress_revisions/h1".rn.t;
SELECT EXISTS (SELECT FROM pg_branch WHERE brname = 'h1') AS h1_used;

-- what isn't there
SELECT * FROM "regress_revisions/nope".rn.t;
SELECT * FROM "regress_revisions/tag1~5".rn.t;
SELECT * FROM "regress_revisions/b1".rn.nope;
SELECT * FROM "regress_revisions/b1".nope.t;
SELECT * FROM "other_db/b1".rn.t;
CREATE TABLE rn.later (k int PRIMARY KEY);
SELECT * FROM "regress_revisions/tag1".rn.later;
DROP TABLE rn.later;

-- only reads: writes, DDL and locks are refused
UPDATE "regress_revisions/b1".rn.t SET v = 0;
INSERT INTO "regress_revisions/b1".rn.t VALUES (99, 0);
ALTER TABLE "regress_revisions/b1".rn.t ADD COLUMN z int;
BEGIN;
LOCK TABLE "regress_revisions/b1".rn.t;
ROLLBACK;

-- the branch's own definitions: a table only it has, a column it added
SELECT dolt_checkout('b1') IS NOT NULL AS on_b1;
CREATE TABLE rn.only_b1 (k text PRIMARY KEY, n int);
INSERT INTO rn.only_b1 VALUES ('a', 1), ('b', 2);
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT * FROM "regress_revisions/b1".rn.only_b1 ORDER BY k;
CREATE VIEW rn_b1 AS SELECT id, v FROM "regress_revisions/b1".rn.t;
PREPARE b1_v(int) AS SELECT v FROM "regress_revisions/b1".rn.t WHERE id = $1;
EXECUTE b1_v(1);
SELECT dolt_checkout('b1') IS NOT NULL AS on_b1;
ALTER TABLE rn.t ADD COLUMN w int DEFAULT 7;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
-- (the session's own definition of the table is as it was)
SELECT * FROM rn.t WHERE id = 1;
SELECT * FROM "regress_revisions/b1".rn.t WHERE id = 1;
-- a view and a plan made before read the revision as it is now
SELECT sum(v) FROM rn_b1;
EXECUTE b1_v(1);
SELECT dolt_checkout('b1') IS NOT NULL AS on_b1;
ALTER TABLE rn.t DROP COLUMN v;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
EXECUTE b1_v(1);
SELECT sum(v) FROM rn_b1;
DEALLOCATE b1_v;
DROP VIEW rn_b1;

-- privileges: the branch's grants on its copy, as a session there has them
CREATE ROLE regress_revisions_reader;
GRANT USAGE ON SCHEMA rn TO regress_revisions_reader;
GRANT SELECT ON rn.t TO regress_revisions_reader;
SET ROLE regress_revisions_reader;
SELECT count(*) FROM rn.t;
SELECT count(*) FROM "regress_revisions/b1".rn.t;
RESET ROLE;
SELECT dolt_checkout('b1') IS NOT NULL AS on_b1;
GRANT USAGE ON SCHEMA rn TO regress_revisions_reader;
GRANT SELECT (id) ON rn.t TO regress_revisions_reader;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_revisions_reader;
SELECT count(id) FROM "regress_revisions/b1".rn.t;
SELECT count(w) FROM "regress_revisions/b1".rn.t;
RESET ROLE;
-- row security on the copy that applies to the reader: refused
SELECT dolt_checkout('b1') IS NOT NULL AS on_b1;
GRANT SELECT ON rn.t TO regress_revisions_reader;
ALTER TABLE rn.t ENABLE ROW LEVEL SECURITY;
CREATE POLICY few ON rn.t USING (id < 3);
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_revisions_reader;
SELECT count(*) FROM "regress_revisions/b1".rn.t;
RESET ROLE;
SELECT count(*) FROM "regress_revisions/b1".rn.t;
-- a commit's table: dolt_at()'s privileges, the table here
REVOKE SELECT ON rn.t FROM regress_revisions_reader;
SET ROLE regress_revisions_reader;
SELECT count(*) FROM "regress_revisions/tag1".rn.t;
RESET ROLE;

-- what was read of b1's catalogs isn't kept as the session's: in a new
-- session, reading b1's copy leaves main's grants on it as they are
\c regress_revisions
SELECT count(*) FROM "regress_revisions/b1".rn.t;
SELECT has_table_privilege('regress_revisions_reader', 'rn.t', 'SELECT') AS main_grants;
SELECT attname FROM pg_attribute WHERE attrelid = 'rn.t'::regclass AND attnum > 0 ORDER BY attnum;

-- names that differ only by case: an exact one is taken, else ambiguous
SELECT dolt_tag('Mixed', 'HEAD') IS NOT NULL, dolt_tag('MIXED', 'HEAD~1') IS NOT NULL;
SELECT sum(v) FROM "regress_revisions/Mixed".rn.t;
SELECT sum(v) FROM "regress_revisions/MIXED".rn.t;
SELECT sum(v) FROM "regress_revisions/mixed".rn.t;

\c contrib_regression
DROP DATABASE regress_revisions;
DROP ROLE regress_revisions_reader;
