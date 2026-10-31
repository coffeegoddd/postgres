-- Extensions are shared or a branch's, never both: one in a shared schema
-- serves every branch, and otherwise each branch may have its own copy of
-- an extension's SQL objects, at its own version.
SELECT pg_branch_id_allocate() AS b1 \gset
SELECT pg_branch_id_allocate() AS b2 \gset
CREATE SCHEMA pub;
SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED;
SET catalog_branch = :b2;
CREATE SCHEMA app VERSIONED;

-- a shared extension serves both branches
SET catalog_branch = :b1;
CREATE EXTENSION hstore SCHEMA pub;
SET catalog_branch = :b2;
SELECT ('a=>1'::pub.hstore) OPERATOR(pub.->) 'a' AS from_b2;
SELECT count(*) FROM pg_extension WHERE extname = 'hstore';
-- and no branch may have its own beside it
CREATE EXTENSION hstore SCHEMA app;
SET catalog_branch = :b1;
DROP EXTENSION hstore;

-- each branch its own, at its own version
CREATE EXTENSION pg_trgm VERSION '1.6' SCHEMA app;
SET catalog_branch = :b2;
CREATE EXTENSION pg_trgm VERSION '1.5' SCHEMA app;
SELECT extversion, extnamespace = 'app'::regnamespace AS in_app
FROM pg_extension WHERE extname = 'pg_trgm';
SELECT round(app.similarity('abc', 'abd')::numeric, 2) AS b2_similarity;
SET catalog_branch = :b1;
SELECT extversion, extnamespace = 'app'::regnamespace AS in_app
FROM pg_extension WHERE extname = 'pg_trgm';
SELECT round(app.similarity('abc', 'abd')::numeric, 2) AS b1_similarity;
SELECT count(*) AS b1_functions FROM pg_proc WHERE proname = 'similarity';

-- a branch updates its own
SET catalog_branch = :b2;
ALTER EXTENSION pg_trgm UPDATE TO '1.6';
SELECT extversion FROM pg_extension WHERE extname = 'pg_trgm';

-- a shared one is refused while a branch has its own, and a session on no
-- branch sees neither branch's
RESET catalog_branch;
CREATE EXTENSION pg_trgm SCHEMA pub;
SELECT count(*) FROM pg_extension WHERE extname = 'pg_trgm';

-- an extension does not move between kinds of schema
SET catalog_branch = :b2;
ALTER EXTENSION pg_trgm SET SCHEMA pub;

-- dropping one branch's leaves the other's
DROP EXTENSION pg_trgm;
SELECT count(*) FROM pg_extension WHERE extname = 'pg_trgm';
SET catalog_branch = :b1;
SELECT extversion FROM pg_extension WHERE extname = 'pg_trgm';
SELECT round(app.similarity('abc', 'abd')::numeric, 2) AS b1_similarity;

-- a branch's extension is that branch's to drop or reassign: DROP OWNED and
-- REASSIGN OWNED on another branch leave it
DROP EXTENSION pg_trgm;
CREATE ROLE regress_ext_owner;
DO $$BEGIN EXECUTE format('GRANT CREATE ON DATABASE %I TO regress_ext_owner', current_database()); END$$;
GRANT CREATE, USAGE ON SCHEMA app TO regress_ext_owner;
SET ROLE regress_ext_owner;
CREATE EXTENSION pg_trgm SCHEMA app;
RESET ROLE;
SET catalog_branch = :b2;
DROP OWNED BY regress_ext_owner;
REASSIGN OWNED BY regress_ext_owner TO CURRENT_USER;
SET catalog_branch = :b1;
SELECT extowner = 'regress_ext_owner'::regrole AS kept FROM pg_extension WHERE extname = 'pg_trgm';
DROP OWNED BY regress_ext_owner;
DO $$BEGIN EXECUTE format('REVOKE CREATE ON DATABASE %I FROM regress_ext_owner', current_database()); END$$;
DROP ROLE regress_ext_owner;
SELECT count(*) AS dropped_on_b1 FROM pg_extension WHERE extname = 'pg_trgm';

DROP SCHEMA app;
SET catalog_branch = :b2;
DROP SCHEMA app;
RESET catalog_branch;
DROP SCHEMA pub;
