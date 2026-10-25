-- A branch switch keeps the caches that are every branch's (catalogs with
-- no branch column, and system catalogs' relation entries) and drops the
-- rest.  Two branches give the same objects (the same OIDs and names)
-- other definitions, privileges, policies, functions, types and
-- comments; each is checked right after a switch, with the caches warm
-- from the other branch, through the paths that read the caches.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
DROP ROLE IF EXISTS regress_bsc_reader;
RESET client_min_messages;
CREATE ROLE regress_bsc_reader;

CREATE SCHEMA bsc VERSIONED;
GRANT USAGE ON SCHEMA bsc TO regress_bsc_reader;
CREATE TYPE bsc.mood AS ENUM ('calm', 'busy');
CREATE TABLE bsc.t (id int PRIMARY KEY, label text NOT NULL, mood bsc.mood);
INSERT INTO bsc.t VALUES (1, 'one', 'calm'), (2, 'two', 'busy');
GRANT SELECT ON bsc.t TO regress_bsc_reader;
ALTER TABLE bsc.t ENABLE ROW LEVEL SECURITY;
CREATE POLICY only_one ON bsc.t FOR SELECT USING (id = 1);
CREATE FUNCTION bsc.f() RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE VIEW bsc.v AS SELECT id, label FROM bsc.t;
COMMENT ON TABLE bsc.t IS 'main''s table';
SELECT length(dolt_commit('-Am', 'bsc main')::text) > 0 AS committed;

-- other: every one of those changed, the objects keeping their OIDs.
SELECT (dolt_checkout('-b', 'bsc_other'))[1];
ALTER TABLE bsc.t RENAME COLUMN label TO name;
ALTER TABLE bsc.t ALTER COLUMN name DROP NOT NULL;
ALTER TABLE bsc.t ADD COLUMN extra int DEFAULT 7;
ALTER TYPE bsc.mood ADD VALUE 'idle';
REVOKE SELECT ON bsc.t FROM regress_bsc_reader;
GRANT SELECT (id) ON bsc.t TO regress_bsc_reader;
DROP POLICY only_one ON bsc.t;
CREATE POLICY only_two ON bsc.t FOR SELECT USING (id = 2);
CREATE OR REPLACE FUNCTION bsc.f() RETURNS int LANGUAGE sql AS 'SELECT 2';
DROP VIEW bsc.v;
CREATE VIEW bsc.v AS SELECT id, name, extra FROM bsc.t;
COMMENT ON TABLE bsc.t IS 'other''s table';
CREATE TABLE bsc.only_other (x int);
SELECT length(dolt_commit('-Am', 'bsc other')::text) > 0 AS committed;

-- What a session sees of them, through the caches: the relation's
-- columns and constraints, the type's labels, privileges, policies (as
-- the reader), the function, the view, the comment and a relation that
-- only one branch has.
CREATE FUNCTION pg_temp.seen() RETURNS TABLE (what text, val text)
LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT 'columns', string_agg(attname || ' ' ||
		format_type(atttypid, atttypmod) || CASE WHEN attnotnull THEN ' not null' ELSE '' END,
		', ' ORDER BY attnum)
		FROM pg_attribute WHERE attrelid = 'bsc.t'::regclass AND attnum > 0
		AND NOT attisdropped;
	RETURN QUERY SELECT 'row', (SELECT row_to_json(t)::text FROM bsc.t t WHERE id = 1);
	RETURN QUERY SELECT 'labels', enum_range(NULL::bsc.mood)::text;
	RETURN QUERY SELECT 'table privilege',
		has_table_privilege('regress_bsc_reader', 'bsc.t', 'SELECT')::text;
	RETURN QUERY SELECT 'column privilege',
		has_column_privilege('regress_bsc_reader', 'bsc.t', 'id', 'SELECT')::text;
	RETURN QUERY SELECT 'policies', string_agg(polname::text, ', ' ORDER BY polname)
		FROM pg_policy WHERE polrelid = 'bsc.t'::regclass;
	RETURN QUERY SELECT 'function', bsc.f()::text;
	RETURN QUERY SELECT 'view', (SELECT row_to_json(v)::text FROM bsc.v v WHERE id = 1);
	RETURN QUERY SELECT 'comment', obj_description('bsc.t'::regclass, 'pg_class');
	RETURN QUERY SELECT 'only_other', (to_regclass('bsc.only_other') IS NOT NULL)::text;
END
$$;
-- (as the reader: the policy and the privileges in force)
CREATE FUNCTION pg_temp.reader_sees() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	SET LOCAL ROLE regress_bsc_reader;
	BEGIN
		SELECT string_agg(id::text, ',' ORDER BY id) INTO r FROM bsc.t;
	EXCEPTION WHEN insufficient_privilege THEN
		r := 'no select';
	END;
	BEGIN
		PERFORM count(label) FROM bsc.t;
		r := r || '; label readable';
	EXCEPTION WHEN insufficient_privilege OR undefined_column THEN
		r := r || '; label not readable';
	END;
	RESET ROLE;
	RETURN r;
END
$$;

-- Each branch twice, the caches warm from the other each time.
SELECT * FROM pg_temp.seen();
SELECT pg_temp.reader_sees();
SELECT (dolt_checkout('main'))[1];
SELECT * FROM pg_temp.seen();
SELECT pg_temp.reader_sees();
SELECT (dolt_checkout('bsc_other'))[1];
SELECT * FROM pg_temp.seen();
SELECT pg_temp.reader_sees();
SELECT (dolt_checkout('main'))[1];
SELECT * FROM pg_temp.seen();
SELECT pg_temp.reader_sees();

-- A prepared statement planned on one branch (the function inlined) is
-- planned again on the other.
PREPARE bsc_q AS SELECT bsc.f(), count(*) FROM bsc.t;
EXECUTE bsc_q;
SELECT (dolt_checkout('bsc_other'))[1];
EXECUTE bsc_q;
SELECT (dolt_checkout('main'))[1];
EXECUTE bsc_q;
DEALLOCATE bsc_q;

-- What every branch shares is kept, and is still right: a role's
-- attribute (pg_authid, shared) changed on one branch is seen on the
-- other.
SELECT (dolt_checkout('bsc_other'))[1];
SELECT rolcanlogin FROM pg_roles WHERE rolname = 'regress_bsc_reader';
ALTER ROLE regress_bsc_reader LOGIN;
SELECT (dolt_checkout('main'))[1];
SELECT rolcanlogin FROM pg_roles WHERE rolname = 'regress_bsc_reader';
SELECT pg_has_role('regress_bsc_reader', 'regress_bsc_reader', 'USAGE');

SELECT dolt_branch('-D', 'bsc_other') IS NOT NULL AS deleted;
DROP SCHEMA bsc CASCADE;
SELECT length(dolt_commit('-Am', 'drop bsc')::text) > 0 AS committed;
DROP ROLE regress_bsc_reader;
