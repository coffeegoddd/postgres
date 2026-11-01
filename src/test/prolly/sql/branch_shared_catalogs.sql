-- Rows of shared catalogs, and of publications, that name a branch's
-- objects are that branch's: a session on another branch does not see
-- them.
SELECT pg_branch_id_allocate() AS b1 \gset
SELECT pg_branch_id_allocate() AS b2 \gset
CREATE ROLE regress_branch_shared;

SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY);
SET catalog_branch = :b2;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, extra int);
ALTER TABLE app.t OWNER TO regress_branch_shared;
SET client_min_messages = error;
CREATE PUBLICATION pub_t FOR TABLE app.t;
-- (an extension whose script revokes privileges records initial ones)
CREATE EXTENSION pg_stat_statements SCHEMA app;
LOAD 'dummy_seclabel';
SECURITY LABEL ON TABLE app.t IS 'classified';
ALTER DEFAULT PRIVILEGES IN SCHEMA app GRANT SELECT ON TABLES TO regress_branch_shared;
SET catalog_branch = :b1;
CREATE PUBLICATION pub_app FOR TABLES IN SCHEMA app;
RESET client_min_messages;

-- pg_shdepend: the role's ownership of b2's table
CREATE TEMP TABLE probes (catalog text, q text);
INSERT INTO probes VALUES
	('pg_shdepend', $$SELECT count(*) FROM pg_shdepend WHERE refobjid = 'regress_branch_shared'::regrole AND classid = 'pg_class'::regclass$$),
	('pg_shdepend (default privileges)', $$SELECT count(*) FROM pg_shdepend WHERE refobjid = 'regress_branch_shared'::regrole AND classid = 'pg_default_acl'::regclass$$),
	('pg_publication_rel', $$SELECT count(*) FROM pg_publication_rel r JOIN pg_publication p ON p.oid = r.prpubid WHERE p.pubname = 'pub_t'$$),
	('pg_publication_namespace', $$SELECT count(*) FROM pg_publication_namespace n JOIN pg_publication p ON p.oid = n.pnpubid WHERE p.pubname = 'pub_app'$$),
	('pg_publication_tables', $$SELECT string_agg(pubname || ':' || tablename, ',' ORDER BY pubname) FROM pg_publication_tables WHERE schemaname = 'app'$$),
	('pg_publication (shared)', $$SELECT count(*) FROM pg_publication WHERE pubname IN ('pub_t', 'pub_app')$$),
	('pg_seclabel', $$SELECT count(*) FROM pg_seclabel WHERE provider = 'dummy' AND label = 'classified'$$),
	('pg_init_privs', $$SELECT count(*) > 0 FROM pg_init_privs i JOIN pg_proc p ON i.classoid = 'pg_proc'::regclass AND p.oid = i.objoid WHERE p.proname = 'pg_stat_statements_reset'$$),
	('pg_init_privs (unjoined)', $$SELECT count(*) > 0 FROM pg_init_privs WHERE classoid = 'pg_proc'::regclass AND objoid >= 16384$$);
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

-- commands that act on what a role owns act on the session's branch's
-- objects and the shared ones: b2's table keeps its owner, and its other
-- table stays, after b1 reassigns and drops what the role owns
SET catalog_branch = :b1;
ALTER TABLE app.t OWNER TO regress_branch_shared;
CREATE TABLE app.d (x int);
ALTER TABLE app.d OWNER TO regress_branch_shared;
SET catalog_branch = :b2;
CREATE TABLE app.d (x int);
ALTER TABLE app.d OWNER TO regress_branch_shared;
SET catalog_branch = :b1;
REASSIGN OWNED BY regress_branch_shared TO CURRENT_USER;
SELECT relname, relowner = 'regress_branch_shared'::regrole AS role_owns
FROM pg_class WHERE relnamespace = 'app'::regnamespace AND relkind = 'r' ORDER BY 1;
SET catalog_branch = :b2;
SELECT relname, relowner = 'regress_branch_shared'::regrole AS role_owns
FROM pg_class WHERE relnamespace = 'app'::regnamespace AND relkind = 'r' ORDER BY 1;
ALTER TABLE app.d OWNER TO regress_branch_shared;
SET catalog_branch = :b1;
ALTER TABLE app.d OWNER TO regress_branch_shared;
DROP OWNED BY regress_branch_shared;
SELECT to_regclass('app.d') IS NULL AS dropped_on_b1;
SET catalog_branch = :b2;
SELECT to_regclass('app.d') IS NOT NULL AS kept_on_b2;

-- setting a publication's tables on one branch leaves another branch's
CREATE TABLE public.branch_shared_s (id int PRIMARY KEY);
SET client_min_messages = error;
SET catalog_branch = :b2;
ALTER PUBLICATION pub_app ADD TABLES IN SCHEMA app;
SET catalog_branch = :b1;
ALTER PUBLICATION pub_t ADD TABLE app.t;
ALTER PUBLICATION pub_t SET TABLE public.branch_shared_s;
ALTER PUBLICATION pub_app SET TABLES IN SCHEMA public;
RESET client_min_messages;
SET catalog_branch_filter = off;
SELECT count(*) AS pub_t_tables FROM pg_publication_rel r
	JOIN pg_publication p ON p.oid = r.prpubid WHERE p.pubname = 'pub_t';
SELECT nspname, nspbranch = :b2 AS on_b2 FROM pg_publication_namespace n
	JOIN pg_publication p ON p.oid = n.pnpubid
	JOIN pg_namespace s ON s.oid = n.pnnspid
WHERE p.pubname = 'pub_app' ORDER BY 1;
RESET catalog_branch_filter;
RESET catalog_branch;
DROP TABLE public.branch_shared_s;

DROP PUBLICATION pub_t, pub_app;
SET catalog_branch = :b2;
ALTER DEFAULT PRIVILEGES IN SCHEMA app REVOKE SELECT ON TABLES FROM regress_branch_shared;
DROP SCHEMA app CASCADE;
SET catalog_branch = :b1;
DROP SCHEMA app CASCADE;
RESET catalog_branch;
DROP ROLE regress_branch_shared;
