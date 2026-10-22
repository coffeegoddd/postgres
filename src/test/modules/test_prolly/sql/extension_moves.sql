-- A versioned schema an extension is in stays on every branch.  A move to a
-- state without it (a reset, a checkout, a branch's first use from such a
-- commit) keeps it, empty but for the extension's objects, which are every
-- branch's; dolt_status then shows it as a schema the commit lacks.  (In a
-- database of its own, so the branches here are the only ones.)

CREATE DATABASE regress_extension_moves;
\c regress_extension_moves

CREATE SCHEMA em VERSIONED;
CREATE TABLE em.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'without ve')::text) AS committed;
SELECT dolt_branch('em_old');			-- head-only, at the commit without ve

CREATE SCHEMA ve VERSIONED;
CREATE EXTENSION citext SCHEMA ve;
CREATE TABLE ve.t (id int PRIMARY KEY, name ve.citext);
INSERT INTO ve.t VALUES (1, 'Hi');
SELECT length(dolt_commit('-Am', 'with ve')::text) AS committed;
SELECT dolt_hashof('HEAD') AS with_ve \gset

-- back to the commit without ve: the table goes, the schema and the
-- extension stay
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT nspname FROM pg_namespace WHERE nspname = 've';
SELECT extname, extnamespace::regnamespace FROM pg_extension WHERE extname = 'citext';
SELECT to_regclass('ve.t') AS t;
SELECT table_name, status FROM dolt_status() ORDER BY 1;
SELECT 'x'::ve.citext::text AS still_usable;

-- forward again
SELECT dolt_reset('--hard', :'with_ve');
SELECT id, name FROM ve.t;

-- a branch's first use, at a commit without ve: it has ve too
SELECT dolt_checkout('em_old');
SELECT nspname FROM pg_namespace WHERE nspname = 've';
CREATE TABLE ve.u (id int PRIMARY KEY, name ve.citext);
INSERT INTO ve.u VALUES (1, 'There');
SELECT dolt_checkout('main');

-- still refused: dropping the schema, or the extension under em_old's table
DROP SCHEMA ve CASCADE;
DROP EXTENSION citext CASCADE;

SELECT dolt_checkout('em_old');
DROP TABLE ve.u;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'em_old');
DROP TABLE ve.t;
DROP EXTENSION citext;
DROP SCHEMA ve, em CASCADE;

\c contrib_regression
DROP DATABASE regress_extension_moves;
