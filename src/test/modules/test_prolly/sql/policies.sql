-- Row security policies on versioned tables: held by a commit with the
-- table (whether row security is on, and forced, and each policy as
-- CREATE POLICY makes it), moved and merged with it, its rows folded and
-- written whatever the policies say.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE ROLE regress_pol_reader;
CREATE SCHEMA po VERSIONED;
CREATE TABLE po.t (id int PRIMARY KEY, owner text, v int);
INSERT INTO po.t SELECT g, CASE WHEN g % 2 = 0 THEN 'even' ELSE 'odd' END, g
  FROM generate_series(1, 10) g;
GRANT USAGE ON SCHEMA po TO regress_pol_reader;
GRANT SELECT ON po.t TO regress_pol_reader;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('po_reset');
SELECT dolt_branch('po_merge');

ALTER TABLE po.t ENABLE ROW LEVEL SECURITY;
ALTER TABLE po.t FORCE ROW LEVEL SECURITY;
CREATE POLICY "even rows" ON po.t FOR SELECT TO regress_pol_reader, PUBLIC
  USING (owner = 'even');
CREATE POLICY small ON po.t AS RESTRICTIVE FOR ALL USING (v < 9) WITH CHECK (v < 100);
-- (names with spaces: a move drops them by name)
CREATE INDEX "my index" ON po.t (v);
CREATE RULE "my rule" AS ON DELETE TO po.t WHERE old.id < 0 DO INSTEAD NOTHING;
SELECT length(dolt_commit('-Am', 'policies')::text) AS committed;

CREATE FUNCTION pg_temp.show() RETURNS SETOF text LANGUAGE sql AS $$
	SELECT 'rls ' || relrowsecurity || ' ' || relforcerowsecurity FROM pg_class
	  WHERE oid = 'po.t'::regclass
	UNION ALL
	SELECT 'policy ' || policyname || ' ' || permissive || ' ' || cmd || ' ' ||
		   array_to_string(roles, ',') || ' ' || coalesce(qual, '-') || ' ' ||
		   coalesce(with_check, '-')
	  FROM pg_policies WHERE schemaname = 'po'
	UNION ALL
	SELECT 'index ' || indexname FROM pg_indexes WHERE schemaname = 'po'
	UNION ALL
	SELECT 'rule ' || rulename FROM pg_rules WHERE schemaname = 'po'
	ORDER BY 1
$$;
CREATE FUNCTION pg_temp.seen() RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(id::text, ',' ORDER BY id) FROM po.t
$$;
SELECT * FROM pg_temp.show();
SET ROLE regress_pol_reader;
SELECT pg_temp.seen() AS reader_sees;
RESET ROLE;

-- a reset from a branch without them makes them from the commit; the
-- rows are all there, whatever the policies show
SELECT dolt_checkout('po_reset');
SELECT dolt_reset('--hard', 'main');
SELECT * FROM dolt_status;
SELECT * FROM pg_temp.show();
SET ROLE regress_pol_reader;
SELECT pg_temp.seen() AS reader_sees;
RESET ROLE;
SET row_security = off;
SELECT count(*) AS all_rows FROM po.t;
RESET row_security;

-- history isn't filtered by policies: its readers refuse a table whose
-- row security applies to them; its owner reads it
SET ROLE regress_pol_reader;
SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::po.t);
SELECT count(*) FROM dolt_conflicts(NULL::po.t);
SELECT count(*) FROM dolt_constraint_violations('po.t'::regclass);
RESET ROLE;
SELECT count(*) AS owner_reads FROM dolt_diff('HEAD', 'WORKING', NULL::po.t);

-- a merge brings them; changed, the change is listed and moves
SELECT dolt_checkout('po_merge');
CREATE TABLE po.other (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'other')::text) AS committed;
SELECT dolt_merge('main') IS NOT NULL AS merged;
SELECT * FROM pg_temp.show();
ALTER POLICY "even rows" ON po.t USING (owner = 'odd');
ALTER TABLE po.t NO FORCE ROW LEVEL SECURITY;
SELECT * FROM dolt_status;
SELECT length(dolt_commit('-Am', 'odd rows')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('po_merge') IS NOT NULL AS merged;
SELECT * FROM pg_temp.show();

-- removed (row security off, the policies, the index and rule with
-- spaces in their names): a move removes them by name
DROP POLICY "even rows" ON po.t;
DROP POLICY small ON po.t;
ALTER TABLE po.t DISABLE ROW LEVEL SECURITY;
DROP INDEX po."my index";
DROP RULE "my rule" ON po.t;
SELECT length(dolt_commit('-Am', 'none')::text) AS committed;
SELECT dolt_checkout('po_reset');
SELECT dolt_reset('--hard', 'main');
SELECT * FROM pg_temp.show();

-- what a move changes under policies: a column's type, a view, a table,
-- a type they use, a column dropped with its policy (each a commit, a
-- branch at the one before reset to it and back)
SELECT dolt_checkout('main');
CREATE TYPE po.mood AS ENUM ('sad', 'happy');
CREATE TABLE po.c (id int PRIMARY KEY, v int, owner text, m po.mood);
CREATE TABLE po.ref (id int PRIMARY KEY);
CREATE VIEW po.cv AS SELECT id FROM po.c WHERE id > 0;
CREATE POLICY small ON po.c USING (v < 9);
CREATE POLICY viewed ON po.c USING (id IN (SELECT id FROM po.cv));
CREATE POLICY refd ON po.c USING (EXISTS (SELECT FROM po.ref r WHERE r.id = c.id));
CREATE POLICY moody ON po.c USING (m = 'happy'::po.mood);
CREATE POLICY owned ON po.c USING (owner = 'x');
SELECT length(dolt_commit('-Am', 'policies over things')::text) AS committed;
SELECT dolt_branch('pc_cases');
DROP POLICY small ON po.c;
ALTER TABLE po.c ALTER COLUMN v TYPE bigint;
CREATE POLICY small ON po.c USING (v < 9);
ALTER TYPE po.mood ADD VALUE 'meh';
DROP POLICY owned ON po.c;
ALTER TABLE po.c DROP COLUMN owner;
DROP POLICY refd ON po.c;
DROP TABLE po.ref;
SELECT length(dolt_commit('-Am', 'things changed')::text) AS committed;
SELECT dolt_checkout('pc_cases');
SELECT dolt_reset('--hard', 'main');
SELECT * FROM dolt_status;
SELECT polname, pg_get_expr(polqual, polrelid) FROM pg_policy
 WHERE polrelid = 'po.c'::regclass ORDER BY 1;
SELECT format_type(atttypid, atttypmod) FROM pg_attribute
 WHERE attrelid = 'po.c'::regclass AND attname = 'v';
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT * FROM dolt_status;
SELECT polname, pg_get_expr(polqual, polrelid) FROM pg_policy
 WHERE polrelid = 'po.c'::regclass ORDER BY 1;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'pc_cases');

SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'po_reset');
SELECT dolt_branch('-D', 'po_merge');
DROP SCHEMA po CASCADE;
SELECT length(dolt_commit('-Am', 'po gone')::text) AS committed;
DROP ROLE regress_pol_reader;
