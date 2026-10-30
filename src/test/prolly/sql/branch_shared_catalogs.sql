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
SET catalog_branch = :b1;
CREATE PUBLICATION pub_app FOR TABLES IN SCHEMA app;
RESET client_min_messages;

-- pg_shdepend: the role's ownership of b2's table
CREATE TEMP TABLE probes (catalog text, q text);
INSERT INTO probes VALUES
	('pg_shdepend', $$SELECT count(*) FROM pg_shdepend WHERE refobjid = 'regress_branch_shared'::regrole AND classid = 'pg_class'::regclass$$),
	('pg_publication_rel', $$SELECT count(*) FROM pg_publication_rel r JOIN pg_publication p ON p.oid = r.prpubid WHERE p.pubname = 'pub_t'$$),
	('pg_publication_namespace', $$SELECT count(*) FROM pg_publication_namespace n JOIN pg_publication p ON p.oid = n.pnpubid WHERE p.pubname = 'pub_app'$$),
	('pg_publication_tables', $$SELECT string_agg(pubname || ':' || tablename, ',' ORDER BY pubname) FROM pg_publication_tables WHERE schemaname = 'app'$$),
	('pg_publication (shared)', $$SELECT count(*) FROM pg_publication WHERE pubname IN ('pub_t', 'pub_app')$$);
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

DROP PUBLICATION pub_t, pub_app;
SET catalog_branch = :b2;
DROP SCHEMA app CASCADE;
SET catalog_branch = :b1;
DROP SCHEMA app CASCADE;
RESET catalog_branch;
DROP ROLE regress_branch_shared;
