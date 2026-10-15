-- Versioned schemas: everything in one is versioned (exists per branch);
-- tables there are prolly tables; shared objects can't depend on versioned
-- ones.

CREATE SCHEMA app VERSIONED;
CREATE SCHEMA plain;
CREATE SCHEMA IF NOT EXISTS app2 VERSIONED;
SELECT nspname, nspversioned FROM pg_namespace
  WHERE nspname IN ('app', 'app2', 'plain', 'public') ORDER BY 1;

-- Tables in a versioned schema are prolly tables, whatever the default.
CREATE TABLE app.t (id int PRIMARY KEY, v text);
SET default_table_access_method = heap;
CREATE TABLE app.t2 (id int);
RESET default_table_access_method;
CREATE TABLE app.t3 (id int) USING prolly;
CREATE TABLE app.p (id int) PARTITION BY RANGE (id);
CREATE TABLE app.p1 PARTITION OF app.p FOR VALUES FROM (0) TO (10);
CREATE MATERIALIZED VIEW app.mv AS SELECT id FROM app.t;
SELECT c.relname, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
  WHERE c.relnamespace = 'app'::regnamespace AND c.relkind IN ('r', 'm', 'p')
  ORDER BY 1;
CREATE TABLE app.h (id int) USING heap;
CREATE UNLOGGED TABLE app.u (id int);
ALTER TABLE app.t SET ACCESS METHOD heap;

-- Prolly tables live only in versioned schemas.
CREATE TABLE plain.p (id int) USING prolly;
SET default_table_access_method = prolly;
CREATE TABLE plain.d (id int);
SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
  WHERE c.oid = 'plain.d'::regclass;
RESET default_table_access_method;
ALTER TABLE plain.d SET ACCESS METHOD prolly;

-- Objects don't move across the boundary.
ALTER TABLE app.t SET SCHEMA plain;
ALTER TABLE plain.d SET SCHEMA app;
CREATE TYPE app.mood AS ENUM ('sad', 'ok', 'happy');
ALTER TYPE app.mood SET SCHEMA plain;

-- Versioned objects may depend on shared ones.
CREATE TABLE plain.base (id int PRIMARY KEY);
CREATE TYPE plain.color AS ENUM ('red', 'blue');
CREATE FUNCTION plain.twice(int) RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT $1 * 2';
CREATE TABLE app.uses (id int DEFAULT plain.twice(21), c plain.color, m app.mood);
CREATE VIEW app.v AS SELECT b.id FROM plain.base b JOIN app.t USING (id);
INSERT INTO app.uses (c, m) VALUES ('red', 'ok');
SELECT * FROM app.uses;

-- Shared objects can't depend on versioned ones.
CREATE TABLE plain.bad1 (m app.mood);
CREATE VIEW plain.bad2 AS SELECT * FROM app.t;
CREATE FUNCTION plain.bad3() RETURNS bigint BEGIN ATOMIC SELECT count(*) FROM app.t; END;
CREATE TABLE plain.bad4 (id int, CHECK (id::text::app.mood IS NOT NULL));
CREATE FUNCTION app.half(int) RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT $1 / 2';
CREATE INDEX ON plain.base (app.half(id));

-- Foreign keys stay on one side.
CREATE TABLE app.child (id int REFERENCES app.t);
CREATE TABLE app.bad5 (id int REFERENCES plain.base);
CREATE TABLE plain.bad6 (id int REFERENCES app.t);

-- A function whose body is a string resolves names when it runs.
CREATE FUNCTION plain.count_t() RETURNS bigint LANGUAGE sql AS 'SELECT count(*) FROM app.t';
INSERT INTO app.t VALUES (1, 'one'), (2, 'two');
SELECT plain.count_t();

-- Extensions are shared.
DROP EXTENSION IF EXISTS tsm_system_rows;
CREATE EXTENSION tsm_system_rows SCHEMA app;
CREATE EXTENSION tsm_system_rows;

DROP SCHEMA app CASCADE;
DROP SCHEMA app2, plain CASCADE;
