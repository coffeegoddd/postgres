-- Work that doesn't grow with the number of branches.  A commit names the
-- branches its transaction wrote by their ids alone (a lookup each), not
-- by listing every branch; DROP ROLE asks whether a commit's hidden
-- branch is left with one probe of pg_branch's name index; and a first
-- use made of the pages a branch keeps as of the head plans the move from
-- that branch alone, however many others sort before it.  branches_listed
-- counts the branches listings of every branch visited.
-- (In a database of its own: other tests' branches would be listed too.)

SELECT current_database() AS home \gset
CREATE DATABASE branch_count_work;
\c branch_count_work
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
CREATE TEMP TABLE runs (branches int, what text, listed int8);
-- (each branch used: only those with copies are listed)
CREATE FUNCTION pg_temp.used(int, int) RETURNS int8 LANGUAGE sql AS $$
	SELECT count(dolt_branch('bcw_' || lpad(g::text, 4, '0'))) +
		   count(dolt_checkout('bcw_' || lpad(g::text, 4, '0')))
	FROM generate_series($1, $2) g
$$;

CREATE SCHEMA bcw VERSIONED;
CREATE TABLE bcw.t (id int PRIMARY KEY, v int);
INSERT INTO bcw.t VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'bcw')::text) > 0 AS committed;
SELECT pg_temp.used(1, 2) > 0 AS made;
SELECT (dolt_checkout('main'))[1];

-- A transaction that wrote main and then bcw_0001, committed on bcw_0001:
-- main is named by its id.
:flush
SELECT pg_temp.since('branches_listed') IS NOT NULL AS counting;
BEGIN;
INSERT INTO bcw.t VALUES (2, 2);
SELECT (dolt_checkout('bcw_0001'))[1];
INSERT INTO bcw.t VALUES (3, 3);
COMMIT;
:flush
INSERT INTO runs SELECT 2, 'commit', pg_temp.since('branches_listed');
SELECT (dolt_checkout('main'))[1];

-- 198 more, and the same again.
SELECT pg_temp.used(3, 200) > 0 AS made;
SELECT (dolt_checkout('main'))[1];
:flush
SELECT pg_temp.since('branches_listed') IS NOT NULL AS counting;
BEGIN;
INSERT INTO bcw.t VALUES (4, 4);
SELECT (dolt_checkout('bcw_0001'))[1];
INSERT INTO bcw.t VALUES (5, 5);
COMMIT;
:flush
INSERT INTO runs SELECT 200, 'commit', pg_temp.since('branches_listed');
SELECT (dolt_checkout('main'))[1];
SELECT count(*) AS rows_on_main FROM bcw.t;

SELECT what, a.listed AS with_2, b.listed AS with_200
FROM runs a JOIN runs b USING (what)
WHERE a.branches = 2 AND b.branches = 200
ORDER BY what;

-- The other branch's unresolved conflicts still refuse the commit: it is
-- still checked, found by its id.
SELECT dolt_branch('bcw_ours') IS NOT NULL AS made;
SELECT dolt_branch('bcw_theirs') IS NOT NULL AS made;
SELECT (dolt_checkout('bcw_theirs'))[1];
UPDATE bcw.t SET v = 30 WHERE id = 1;
SELECT length(dolt_commit('-am', 'theirs')::text) > 0 AS committed;
SELECT (dolt_checkout('bcw_ours'))[1];
UPDATE bcw.t SET v = 20 WHERE id = 1;
SELECT length(dolt_commit('-am', 'ours')::text) > 0 AS committed;
SELECT (dolt_checkout('main'))[1];
BEGIN;
SELECT (dolt_checkout('bcw_ours'))[1];
SELECT (dolt_merge('bcw_theirs'))[3] AS conflicts;
SELECT (dolt_checkout('main'))[1];
COMMIT;

-- DROP ROLE, with something depending on the role, asks whether a hidden
-- branch is left: by one probe of the name index, not a scan of pg_branch.
CREATE ROLE regress_bcw_owner;
CREATE TABLE bcw_owned (id int);
ALTER TABLE bcw_owned OWNER TO regress_bcw_owner;
:flush
SELECT seq_scan AS seq_before FROM pg_stat_sys_tables WHERE relname = 'pg_branch' \gset
DROP ROLE regress_bcw_owner;
:flush
SELECT seq_scan - :seq_before AS pg_branch_scans
FROM pg_stat_sys_tables WHERE relname = 'pg_branch';
DROP TABLE bcw_owned;
DROP ROLE regress_bcw_owner;

-- A first use made of the pages bcw_0200 keeps as of its head, from main:
-- the move from bcw_0200 is planned (for the tables to begin empty),
-- though 199 branches with copies sort before it.
SELECT (dolt_checkout('bcw_0200'))[1];
INSERT INTO bcw.t VALUES (200, 200);
SELECT length(dolt_commit('-am', 'on 200')::text) > 0 AS committed;
UPDATE bcw.t SET v = -1;
SELECT (dolt_checkout('main'))[1];
SELECT dolt_branch('bcw_from_200', 'bcw_0200') IS NOT NULL AS made;
:flush
SELECT pg_temp.since('move_plans') IS NOT NULL AS counting;
SELECT pg_temp.since('first_use_tables_kept') IS NOT NULL AS counting;
SELECT (dolt_checkout('bcw_from_200'))[1];
:flush
SELECT pg_temp.since('first_use_tables_kept') AS tables_kept,
	pg_temp.since('move_plans') AS move_plans;
SELECT count(*) AS rows_at_head, count(*) FILTER (WHERE v = -1) AS dirty FROM bcw.t;
SELECT (dolt_checkout('main'))[1];

\c :home
DROP DATABASE branch_count_work;
