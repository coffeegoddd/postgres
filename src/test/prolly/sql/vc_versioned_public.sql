--
-- Included by the version-control tests: make public a versioned schema,
-- if it isn't one, for branches to hold their tables.
--
-- Branches hold the tables of versioned schemas: public is one.
SELECT NOT nspversioned AS vc_make_versioned FROM pg_namespace
WHERE nspname = 'public' \gset
\if :vc_make_versioned
DROP SCHEMA public CASCADE;
CREATE SCHEMA public VERSIONED;
\endif
