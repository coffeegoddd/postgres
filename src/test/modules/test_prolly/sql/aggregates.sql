-- Aggregates in versioned schemas: held by a commit (their definition,
-- every option), made by a move after the functions they use, dropped
-- before them, merged and listed as other routines are.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA ag VERSIONED;
CREATE TABLE ag.t (id int PRIMARY KEY, n int, g text);
INSERT INTO ag.t SELECT g, g % 7, 'g' || (g % 3) FROM generate_series(1, 30) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('ag_reset');
SELECT dolt_branch('ag_merge');

-- a user function as the state function, a final one, a combine one
CREATE FUNCTION ag.step(state int8, v int) RETURNS int8 LANGUAGE sql
  IMMUTABLE AS 'SELECT state + v * v';
CREATE FUNCTION ag.done(state int8) RETURNS numeric LANGUAGE sql
  IMMUTABLE AS 'SELECT sqrt(state)';
CREATE AGGREGATE ag.norm(int) (
	SFUNC = ag.step, STYPE = int8, FINALFUNC = ag.done, INITCOND = '0',
	COMBINEFUNC = int8pl, PARALLEL = SAFE);
-- built-in support, a moving form and a sort operator
CREATE AGGREGATE ag.total(int) (
	SFUNC = int4pl, STYPE = int4, INITCOND = '0',
	MSFUNC = int4pl, MINVFUNC = int4mi, MSTYPE = int4, MINITCOND = '0');
CREATE AGGREGATE ag.least_of(int) (SFUNC = int4smaller, STYPE = int4, SORTOP = <);
-- ordered-set and hypothetical
CREATE AGGREGATE ag.pct(float8 ORDER BY int) (
	SFUNC = ordered_set_transition, STYPE = internal,
	FINALFUNC = percentile_disc_final, FINALFUNC_EXTRA);
CREATE AGGREGATE ag.rnk(VARIADIC "any" ORDER BY VARIADIC "any") (
	SFUNC = ordered_set_transition_multi, STYPE = internal,
	FINALFUNC = rank_final, FINALFUNC_EXTRA, HYPOTHETICAL);
SELECT length(dolt_commit('-Am', 'aggregates')::text) AS committed;

CREATE FUNCTION pg_temp.show() RETURNS SETOF text LANGUAGE sql AS $$
	SELECT 'norm ' || ag.norm(n)::text FROM ag.t
	UNION ALL
	SELECT 'total ' || ag.total(n)::text FROM ag.t
	UNION ALL
	SELECT 'moving ' || string_agg(m::text, ',') FROM
	  (SELECT ag.total(n) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS m
	   FROM ag.t WHERE id <= 5) w
	UNION ALL
	SELECT 'least ' || ag.least_of(n)::text FROM ag.t
	UNION ALL
	SELECT 'pct ' || ag.pct(0.5) WITHIN GROUP (ORDER BY n)::text FROM ag.t
	UNION ALL
	SELECT 'rnk ' || ag.rnk(3) WITHIN GROUP (ORDER BY n)::text FROM ag.t
	ORDER BY 1
$$;
SELECT * FROM pg_temp.show();
SELECT p.proname, a.aggkind, a.aggsortop <> 0 AS sorted, a.aggmtransfn <> 0 AS moving
  FROM pg_aggregate a JOIN pg_proc p ON p.oid = a.aggfnoid
  WHERE p.pronamespace = 'ag'::regnamespace ORDER BY 1;

-- a reset from a branch without them makes them from the commit
SELECT dolt_checkout('ag_reset');
SELECT dolt_reset('--hard', 'main');
SELECT * FROM dolt_status;
SELECT * FROM pg_temp.show();

-- a merge brings them; one changed moves as a whole
SELECT dolt_checkout('ag_merge');
CREATE TABLE ag.other (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'other')::text) AS committed;
SELECT dolt_merge('main') IS NOT NULL AS merged;
SELECT * FROM pg_temp.show();
CREATE OR REPLACE AGGREGATE ag.total(int) (
	SFUNC = int4pl, STYPE = int4, INITCOND = '100',
	MSFUNC = int4pl, MINVFUNC = int4mi, MSTYPE = int4, MINITCOND = '0');
SELECT * FROM dolt_status;
SELECT length(dolt_commit('-Am', 'total from 100')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('ag_merge') IS NOT NULL AS merged;
SELECT ag.total(n) FROM ag.t;

-- dropped, with the functions they use: the aggregates go first
DROP AGGREGATE ag.norm(int);
DROP FUNCTION ag.step(int8, int);
DROP FUNCTION ag.done(int8);
SELECT length(dolt_commit('-Am', 'no norm')::text) AS committed;
SELECT dolt_checkout('ag_reset');
SELECT dolt_reset('--hard', 'main');
SELECT count(*) AS norm FROM pg_proc WHERE proname IN ('norm', 'step', 'done')
  AND pronamespace = 'ag'::regnamespace;

SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'ag_reset');
SELECT dolt_branch('-D', 'ag_merge');
DROP SCHEMA ag CASCADE;
SELECT length(dolt_commit('-Am', 'ag gone')::text) AS committed;
