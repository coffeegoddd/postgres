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
SELECT (dolt_commit('-Am', 'key and routine'))[1] AS c2 \gset

-- back across the key: the routine goes before it
SELECT dolt_reset('--hard', :'c1');
SELECT dolt_hashof('HEAD') = :'c1' AS at_first;
SELECT to_regprocedure('kr.f()') IS NULL AS no_routine,
	(SELECT count(*) FROM pg_constraint WHERE conrelid = 'kr.t'::regclass
		AND contype = 'p') AS keys;
-- and forward again: the routine is made once the key is
SELECT dolt_reset('--hard', :'c2');
SELECT dolt_hashof('HEAD') = :'c2' AS at_second;
SELECT * FROM kr.f();

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

-- the table and the routine going together
DROP FUNCTION kr.f();
DROP TABLE kr.t;
SELECT (dolt_commit('-am', 'both gone'))[1] AS c4 \gset
SELECT dolt_reset('--hard', :'c3');
SELECT * FROM kr.f();
SELECT dolt_reset('--hard', :'c4');
SELECT to_regclass('kr.t') IS NULL AS no_table, to_regprocedure('kr.f()') IS NULL AS no_routine;
SELECT * FROM dolt_status();

DROP SCHEMA kr CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) > 0 AS committed;
