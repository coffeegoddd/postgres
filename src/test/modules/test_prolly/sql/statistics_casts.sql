-- Extended statistics and casts of versioned types, held by commits: a
-- table's statistics in its definition, a cast as an item of its own; a
-- move makes, changes and drops them.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA sc VERSIONED;
CREATE TABLE sc.t (id int PRIMARY KEY, a int, b int, c text);
SELECT length(dolt_commit('-Am', 'table')::text) AS committed;
SELECT dolt_branch('sc_other');

CREATE STATISTICS sc.st (dependencies, ndistinct) ON a, b FROM sc.t;
CREATE STATISTICS sc."st 2" ON (a + b), c FROM sc.t;
ALTER STATISTICS sc."st 2" SET STATISTICS 50;
CREATE TYPE sc.e AS ENUM ('x', 'y');
CREATE FUNCTION sc.e_int(sc.e) RETURNS int LANGUAGE sql IMMUTABLE
  AS 'SELECT CASE $1 WHEN ''x'' THEN 1 ELSE 2 END';
CREATE CAST (sc.e AS int) WITH FUNCTION sc.e_int(sc.e) AS ASSIGNMENT;
CREATE CAST (sc.e AS text) WITH INOUT AS IMPLICIT;
SELECT * FROM dolt_status ORDER BY table_name COLLATE "C";
SELECT length(dolt_commit('-Am', 'held')::text) AS committed;
SELECT * FROM dolt_status;

CREATE FUNCTION pg_temp.show() RETURNS SETOF text LANGUAGE sql AS $$
	SELECT x FROM (
	SELECT 'stat ' || pg_get_statisticsobjdef(oid) || ' target ' ||
		   coalesce(stxstattarget::text, '-')
	  FROM pg_statistic_ext WHERE stxnamespace = 'sc'::regnamespace
	UNION ALL
	SELECT 'cast ' || castsource::regtype || ' as ' || casttarget::regtype ||
		   ' ' || castmethod::text || castcontext::text || ' ' || castfunc::regproc
	  FROM pg_cast WHERE castsource = 'sc.e'::regtype) s(x)
	ORDER BY x COLLATE "C"
$$;
SELECT * FROM pg_temp.show();

-- a reset from a branch without them makes them; back drops them
SELECT dolt_checkout('sc_other');
SELECT dolt_reset('--hard', 'main');
SELECT * FROM dolt_status;
SELECT * FROM pg_temp.show();
SELECT 'x'::sc.e::int AS cast_works;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT * FROM dolt_status;
SELECT count(*) AS statistics FROM pg_statistic_ext WHERE stxnamespace = 'sc'::regnamespace;
SELECT count(*) AS casts FROM pg_cast WHERE castsource::regtype::text LIKE 'sc.%';

-- changed: a statistics target, a cast's context; merged
SELECT dolt_checkout('main');
ALTER STATISTICS sc."st 2" SET STATISTICS 75;
DROP CAST (sc.e AS text);
CREATE CAST (sc.e AS text) WITH INOUT AS ASSIGNMENT;
DROP STATISTICS sc.st;
SELECT * FROM dolt_status ORDER BY table_name COLLATE "C";
SELECT length(dolt_commit('-am', 'changed')::text) AS committed;
SELECT dolt_checkout('sc_other');
SELECT dolt_merge('main') IS NOT NULL AS merged;
SELECT * FROM dolt_status;
SELECT * FROM pg_temp.show();

SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'sc_other');
DROP SCHEMA sc CASCADE;
SELECT length(dolt_commit('-Am', 'sc gone')::text) AS committed;
