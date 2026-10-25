-- A routine with a SQL-standard body may select columns a primary key
-- determines, grouping by the key: it depends on the key, which can't go
-- while it is there.  A move that drops the key drops such a routine
-- first, and makes it again if the commit it moves to keeps it.

CREATE SCHEMA kr VERSIONED;
CREATE TABLE kr.t (id int NOT NULL, a int);
INSERT INTO kr.t VALUES (1, 10), (2, 20);
SELECT (dolt_commit('-Am', 'no key'))[1] AS c1 \gset
ALTER TABLE kr.t ADD PRIMARY KEY (id);
CREATE FUNCTION kr.f() RETURNS SETOF int LANGUAGE sql
	BEGIN ATOMIC SELECT a FROM kr.t GROUP BY id ORDER BY id; END;
-- (one with a named argument too)
CREATE FUNCTION kr.g(x int) RETURNS SETOF int LANGUAGE sql
	BEGIN ATOMIC SELECT a + x FROM kr.t GROUP BY id ORDER BY id; END;
SELECT (dolt_commit('-Am', 'key and routines'))[1] AS c2 \gset

-- back across the key: the routine goes before it
SELECT dolt_reset('--hard', :'c1');
SELECT dolt_hashof('HEAD') = :'c1' AS at_first;
SELECT to_regprocedure('kr.f()') IS NULL AND to_regprocedure('kr.g(int)') IS NULL
		AS no_routines,
	(SELECT count(*) FROM pg_constraint WHERE conrelid = 'kr.t'::regclass
		AND contype = 'p') AS keys;
-- and forward again: the routine is made once the key is
SELECT dolt_reset('--hard', :'c2');
SELECT dolt_hashof('HEAD') = :'c2' AS at_second;
SELECT * FROM kr.f();
SELECT * FROM kr.g(1);

-- the key renamed, the routine kept: it goes before the old key, and
-- comes again on the new one
ALTER TABLE kr.t RENAME CONSTRAINT t_pkey TO t_key;
SELECT (dolt_commit('-am', 'key renamed'))[1] AS c3 \gset
SELECT dolt_reset('--hard', :'c2');
SELECT conname FROM pg_constraint WHERE conrelid = 'kr.t'::regclass AND contype = 'p';
SELECT * FROM kr.f();
SELECT dolt_reset('--hard', :'c3');
SELECT conname FROM pg_constraint WHERE conrelid = 'kr.t'::regclass AND contype = 'p';
SELECT * FROM kr.f();
SELECT c.conname AS routine_uses
	FROM pg_depend d JOIN pg_constraint c ON c.oid = d.refobjid
	WHERE d.objid = 'kr.f()'::regprocedure AND d.refclassid = 'pg_constraint'::regclass;

-- the table and the routines going together
DROP FUNCTION kr.f();
DROP FUNCTION kr.g(int);
DROP TABLE kr.t;
SELECT (dolt_commit('-am', 'both gone'))[1] AS c4 \gset
SELECT dolt_reset('--hard', :'c3');
SELECT * FROM kr.f();
SELECT dolt_reset('--hard', :'c4');
SELECT to_regclass('kr.t') IS NULL AS no_table, to_regprocedure('kr.f()') IS NULL AS no_routine;
SELECT * FROM dolt_status();

DROP SCHEMA kr CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) > 0 AS committed;

-- A routine kept across the key's change is the same routine after: its
-- OID, owner, grants, comment and SECURITY DEFINER kept, and what uses it
-- (another routine, a column default, a materialized view) left as it
-- is.  A materialized view grouping by the key goes and comes again.
CREATE ROLE regress_kr_owner;
CREATE ROLE regress_kr_user;
CREATE SCHEMA ko VERSIONED;
GRANT USAGE ON SCHEMA ko TO regress_kr_owner;
CREATE TABLE ko.t (id int PRIMARY KEY, a int);
INSERT INTO ko.t VALUES (1, 10), (2, 20);
GRANT SELECT ON ko.t TO regress_kr_owner;
CREATE FUNCTION ko.f() RETURNS SETOF int LANGUAGE sql SECURITY DEFINER
	SET search_path = ko
	BEGIN ATOMIC SELECT a FROM ko.t GROUP BY id ORDER BY id; END;
ALTER FUNCTION ko.f() OWNER TO regress_kr_owner;
REVOKE EXECUTE ON FUNCTION ko.f() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION ko.f() TO regress_kr_user;
COMMENT ON FUNCTION ko.f() IS 'grouped by the key';
CREATE FUNCTION ko.g() RETURNS bigint LANGUAGE sql
	BEGIN ATOMIC SELECT count(*) FROM ko.f(); END;
CREATE TABLE ko.u (id int PRIMARY KEY, n bigint DEFAULT ko.g());
CREATE MATERIALIZED VIEW ko.calls AS SELECT * FROM ko.f() x;
CREATE MATERIALIZED VIEW ko.grouped AS SELECT id, a FROM ko.t GROUP BY id;
SELECT (dolt_commit('-Am', 'routines on the key'))[1] AS k1 \gset
SELECT 'ko.f()'::regprocedure::oid AS f_oid \gset
ALTER TABLE ko.t RENAME CONSTRAINT t_pkey TO t_key;
SELECT (dolt_commit('-am', 'key renamed'))[1] AS k2 \gset
SELECT dolt_reset('--hard', :'k1');
SELECT dolt_reset('--hard', :'k2');
SELECT dolt_reset('--hard', :'k1');
SELECT dolt_hashof('HEAD') = :'k1' AS at_first;
SELECT oid = :f_oid AS same_routine, proowner::regrole, proacl, prosecdef,
	proconfig, obj_description(oid, 'pg_proc') AS comment
	FROM pg_proc WHERE oid = 'ko.f()'::regprocedure;
SELECT ko.g(), (SELECT count(*) FROM ko.calls) AS calls,
	(SELECT count(*) FROM ko.grouped) AS grouped;
INSERT INTO ko.u (id) VALUES (1);
SELECT * FROM ko.u;
SELECT * FROM dolt_status();
SELECT dolt_reset('--hard');

-- A table made again (a column added before those it keeps) takes with it
-- what reads it: a routine reading it, one grouping by its key.
CREATE TABLE ko.r (id int PRIMARY KEY, a int);
INSERT INTO ko.r VALUES (1, 5);
CREATE FUNCTION ko.reads() RETURNS SETOF int LANGUAGE sql
	BEGIN ATOMIC SELECT a FROM ko.r; END;
CREATE FUNCTION ko.groups() RETURNS SETOF int LANGUAGE sql
	BEGIN ATOMIC SELECT a FROM ko.r GROUP BY id; END;
SELECT (dolt_commit('-Am', 'r'))[1] AS r1 \gset
DROP FUNCTION ko.reads();
DROP FUNCTION ko.groups();
DROP TABLE ko.r;
CREATE TABLE ko.r (z int, id int PRIMARY KEY, a int);
INSERT INTO ko.r VALUES (0, 1, 5);
CREATE FUNCTION ko.reads() RETURNS SETOF int LANGUAGE sql
	BEGIN ATOMIC SELECT a FROM ko.r; END;
CREATE FUNCTION ko.groups() RETURNS SETOF int LANGUAGE sql
	BEGIN ATOMIC SELECT a FROM ko.r GROUP BY id; END;
SELECT (dolt_commit('-Am', 'r made again'))[1] AS r2 \gset
SELECT dolt_reset('--hard', :'r1');
SELECT attname FROM pg_attribute WHERE attrelid = 'ko.r'::regclass AND attnum > 0
	AND NOT attisdropped ORDER BY attnum;
SELECT * FROM ko.reads(), ko.groups();
SELECT dolt_reset('--hard', :'r2');
SELECT attname FROM pg_attribute WHERE attrelid = 'ko.r'::regclass AND attnum > 0
	AND NOT attisdropped ORDER BY attnum;
SELECT * FROM ko.reads(), ko.groups();
SELECT * FROM dolt_status();

DROP SCHEMA ko CASCADE;
SELECT length(dolt_commit('-Am', 'without ko')::text) > 0 AS committed;

-- A routine emptied of its body that can only be made again once a view
-- the move makes again is there is made before what calls it is built or
-- checked: a re-made table's CHECK, expression index and generated
-- column.  The placeholder keeps its volatility (IMMUTABLE, LEAKPROOF
-- for a superuser), so the generated column can be made meanwhile.
CREATE SCHEMA kv VERSIONED;
CREATE TABLE kv.t (id int PRIMARY KEY, a int);
INSERT INTO kv.t VALUES (1, 10), (2, 20);
CREATE VIEW kv.v AS SELECT id, a FROM kv.t;
CREATE FUNCTION kv.lk(x int) RETURNS int LANGUAGE sql IMMUTABLE STRICT LEAKPROOF
	BEGIN ATOMIC SELECT t.a FROM kv.t JOIN kv.v USING (id) WHERE t.id = x GROUP BY t.id; END;
CREATE TABLE kv.u (x int CHECK (kv.lk(x) IS NOT NULL),
	y int GENERATED ALWAYS AS (kv.lk(x)) STORED);
CREATE INDEX ON kv.u ((kv.lk(x)));
INSERT INTO kv.u VALUES (1), (2);
SELECT (dolt_commit('-Am', 'v1'))[1] AS v1 \gset
ALTER TABLE kv.t RENAME CONSTRAINT t_pkey TO t_key;
CREATE OR REPLACE VIEW kv.v AS SELECT id, a, 1 AS one FROM kv.t;
ALTER TABLE kv.u RENAME TO u_old;
CREATE TABLE kv.u (z int, x int CHECK (kv.lk(x) IS NOT NULL),
	y int GENERATED ALWAYS AS (kv.lk(x)) STORED);
CREATE INDEX ON kv.u ((kv.lk(x)));
INSERT INTO kv.u (z, x) SELECT 0, x FROM kv.u_old;
DROP TABLE kv.u_old;
SELECT (dolt_commit('-Am', 'v2'))[1] AS v2 \gset
SELECT dolt_reset('--hard', :'v1');
SELECT dolt_hashof('HEAD') = :'v1' AS at_v1, provolatile, proisstrict, proleakproof
	FROM pg_proc WHERE oid = 'kv.lk(int)'::regprocedure;
SELECT x, y FROM kv.u ORDER BY x;
SELECT dolt_reset('--hard', :'v2');
SELECT dolt_hashof('HEAD') = :'v2' AS at_v2;
SELECT z, x, y FROM kv.u ORDER BY x;
SELECT * FROM dolt_status();
DROP SCHEMA kv CASCADE;
SELECT length(dolt_commit('-Am', 'without kv')::text) > 0 AS committed;

-- A routine whose argument or result is a re-made table's row type is
-- dropped and made again (its signature can't stay), its owner,
-- privileges and comment given back; one with a SQL-standard body and one
-- in PL/pgSQL.
CREATE SCHEMA kt VERSIONED;
GRANT USAGE ON SCHEMA kt TO regress_kr_owner;
CREATE TABLE kt.t (z int, id int PRIMARY KEY, a int);
INSERT INTO kt.t VALUES (0, 1, 10);
CREATE FUNCTION kt.rows() RETURNS SETOF kt.t LANGUAGE sql BEGIN ATOMIC SELECT * FROM kt.t; END;
CREATE FUNCTION kt.first(r kt.t) RETURNS int LANGUAGE plpgsql AS $$BEGIN RETURN r.a; END$$;
ALTER FUNCTION kt.rows() OWNER TO regress_kr_owner;
REVOKE EXECUTE ON FUNCTION kt.rows() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION kt.rows() TO regress_kr_user;
COMMENT ON FUNCTION kt.first(kt.t) IS 'a row''s a';
SELECT (dolt_commit('-Am', 't1'))[1] AS t1 \gset
DROP FUNCTION kt.rows();
DROP FUNCTION kt.first(kt.t);
ALTER TABLE kt.t DROP COLUMN z;
CREATE FUNCTION kt.rows() RETURNS SETOF kt.t LANGUAGE sql BEGIN ATOMIC SELECT * FROM kt.t; END;
CREATE FUNCTION kt.first(r kt.t) RETURNS int LANGUAGE plpgsql AS $$BEGIN RETURN r.a; END$$;
ALTER FUNCTION kt.rows() OWNER TO regress_kr_owner;
REVOKE EXECUTE ON FUNCTION kt.rows() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION kt.rows() TO regress_kr_user;
COMMENT ON FUNCTION kt.first(kt.t) IS 'a row''s a';
SELECT (dolt_commit('-Am', 't2'))[1] AS t2 \gset
-- (t made again: z added before the columns it keeps)
SELECT dolt_reset('--hard', :'t1');
SELECT dolt_hashof('HEAD') = :'t1' AS at_t1;
SELECT p.proname, nullif(p.proowner::regrole::text, current_user) AS proowner, p.proacl, obj_description(p.oid, 'pg_proc') AS comment
	FROM pg_proc p WHERE p.pronamespace = 'kt'::regnamespace ORDER BY 1;
SELECT (r).z, kt.first(r) FROM kt.rows() r;
SELECT dolt_reset('--hard', :'t2');
SELECT dolt_hashof('HEAD') = :'t2' AS at_t2;
SELECT kt.first(r) FROM kt.rows() r;
SELECT * FROM dolt_status();
DROP SCHEMA kt CASCADE;
SELECT length(dolt_commit('-Am', 'without kt')::text) > 0 AS committed;

-- A column the move drops in place: a routine reading it first emptied of
-- its body (and here dropped with it, as the commit has neither).
CREATE SCHEMA kc VERSIONED;
CREATE TABLE kc.t (id int PRIMARY KEY, a int);
INSERT INTO kc.t VALUES (1, 10);
SELECT (dolt_commit('-Am', 'c1'))[1] AS kc1 \gset
ALTER TABLE kc.t ADD COLUMN b int;
CREATE FUNCTION kc.f() RETURNS SETOF int LANGUAGE sql BEGIN ATOMIC SELECT b FROM kc.t; END;
SELECT (dolt_commit('-Am', 'c2'))[1] AS kc2 \gset
SELECT dolt_reset('--hard', :'kc1');
SELECT dolt_hashof('HEAD') = :'kc1' AS at_c1, to_regprocedure('kc.f()') IS NULL AS no_routine;
SELECT dolt_reset('--hard', :'kc2');
SELECT * FROM kc.f();
DROP SCHEMA kc CASCADE;
SELECT length(dolt_commit('-Am', 'without kc')::text) > 0 AS committed;
DROP ROLE regress_kr_owner;
DROP ROLE regress_kr_user;
