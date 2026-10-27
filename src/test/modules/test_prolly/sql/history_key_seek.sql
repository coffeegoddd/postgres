-- dolt_history_table(NULL::t) with quals on the table's primary key: each
-- commit's data tree is sought for the keys the quals bound, rather than
-- every row of every commit read, and a commit whose table is the tree the
-- commit before it had is given that commit's rows, reading nothing.  So a
-- key's history reads the commits times the trees' depth, not their rows.
-- The rows are the function's own: the same as a qual the scan can't bound
-- ((row).id + 0 = 5) gives, through schema changes (read whole), a
-- column renamed, a keyless table (never bounded) and the history horizon.

SELECT current_database() AS home \gset
\set VERBOSITY terse
CREATE DATABASE regress_history_key_seek;
\c regress_history_key_seek

-- (the tree nodes a statement reads, its counts flushed)
CREATE FUNCTION nodes_read() RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = 'nodes_read'), 0)
$$;
\set flush 'SELECT pg_stat_force_next_flush() IS NULL AS flushed \\gset'

CREATE SCHEMA hk VERSIONED;
CREATE TABLE hk.t (id int PRIMARY KEY, v text NOT NULL);
INSERT INTO hk.t SELECT g, repeat('v', 100) || g FROM generate_series(1, 4000) g;
SELECT dolt_commit('-Am', 'load') IS NOT NULL AS committed;
-- 40 commits, each changing id 5 and another row; 10 more changing
-- another table only
CREATE TABLE hk.other (id int PRIMARY KEY);
CREATE FUNCTION hk_history(n int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..n LOOP
		UPDATE hk.t SET v = 'r' || i WHERE id IN (5, 100 * i);
		PERFORM dolt_commit('-Am', 'r' || i);
	END LOOP;
	FOR i IN 1..10 LOOP
		INSERT INTO hk.other VALUES (i);
		PERFORM dolt_commit('-Am', 'o' || i);
	END LOOP;
	RETURN n;
END $$;
SELECT hk_history(40) AS commits;

-- the plan: the quals bound the key, shown, and stay the filter
EXPLAIN (COSTS OFF) SELECT (row).v FROM dolt_history_table(NULL::hk.t) WHERE (row).id = 5;
EXPLAIN (COSTS OFF) SELECT (row).v FROM dolt_history_table(NULL::hk.t)
WHERE (row).id IN (5, 7) AND commit_hash IS NOT NULL;
EXPLAIN (COSTS OFF) SELECT (row).v FROM dolt_history_table(NULL::hk.t)
WHERE (row).id > 3990 AND (row).id <= 3995;
-- (a qual not on the key's column itself isn't a bound)
EXPLAIN (COSTS OFF) SELECT (row).v FROM dolt_history_table(NULL::hk.t) WHERE (row).id + 0 = 5;

-- the same rows as the function gives
CREATE FUNCTION same(bounded text, whole text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, %L ORDER BY r::text) FROM (%s) r', ',', bounded) INTO a;
	EXECUTE format('SELECT string_agg(r::text, %L ORDER BY r::text) FROM (%s) r', ',', whole) INTO b;
	RETURN a IS NOT DISTINCT FROM b AND a IS NOT NULL;
END $$;
SELECT same($$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.t) WHERE (row).id = 5$$,
			$$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.t) WHERE (row).id + 0 = 5$$) AS eq;
SELECT same($$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.t) WHERE (row).id IN (5, 700, 99999)$$,
			$$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.t) WHERE (row).id + 0 IN (5, 700, 99999)$$) AS in_list;
SELECT same($$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.t) WHERE (row).id > 3990 AND (row).id <= 3995$$,
			$$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.t) WHERE (row).id + 0 > 3990 AND (row).id + 0 <= 3995$$) AS range;
SELECT same($$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.t) WHERE (row).id < 3$$,
			$$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.t) WHERE (row).id + 0 < 3$$) AS below;
SELECT count(*) AS rows_of_5 FROM dolt_history_table(NULL::hk.t) WHERE (row).id = 5;
SELECT count(*) AS no_such_key FROM dolt_history_table(NULL::hk.t) WHERE (row).id = 99999;
-- (a parameter, NULL, and an outer row's value)
PREPARE at_key(int) AS SELECT count(*) FROM dolt_history_table(NULL::hk.t) WHERE (row).id = $1;
EXECUTE at_key(5);
EXECUTE at_key(NULL);
SELECT k.id, (SELECT count(*) FROM dolt_history_table(NULL::hk.t) h WHERE (h.row).id = k.id) AS rows
FROM (VALUES (5), (300), (99999)) k(id) ORDER BY k.id;

-- The reads: a seek a commit, O(the tree's depth), for the 40 commits
-- that changed the table; none for the 10 that didn't, nor the load's
-- version again.  The whole read reads every node of every version.
:flush
SELECT nodes_read() AS before \gset
SELECT count(*) AS rows_of_5 FROM dolt_history_table(NULL::hk.t) WHERE (row).id = 5;
:flush
SELECT nodes_read() - :before AS bounded \gset
SELECT nodes_read() AS before \gset
SELECT count(*) AS rows_of_5 FROM dolt_history_table(NULL::hk.t) WHERE (row).id + 0 = 5;
:flush
SELECT nodes_read() - :before AS whole \gset
SELECT :bounded <= 51 * 16 AS commits_times_depth, :whole > 10 * :bounded AS whole_reads_more;

-- A column added, one renamed: the versions before are defined otherwise,
-- read whole; the rows are the function's still.
ALTER TABLE hk.t ADD COLUMN w int;
UPDATE hk.t SET w = 1 WHERE id = 5;
SELECT dolt_commit('-am', 'w') IS NOT NULL AS committed;
ALTER TABLE hk.t RENAME COLUMN v TO v2;
UPDATE hk.t SET v2 = 'renamed' WHERE id = 5;
SELECT dolt_commit('-am', 'renamed') IS NOT NULL AS committed;
SELECT same($$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.t) WHERE (row).id = 5$$,
			$$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.t) WHERE (row).id + 0 = 5$$) AS across_changes;
SELECT (row).v2, (row).w FROM dolt_history_table(NULL::hk.t) WHERE (row).id = 5
ORDER BY commit_date DESC, (row).v2 LIMIT 3;

-- A primary key of two columns: its first, then a range on its second.
CREATE TABLE hk.p (a int, b int, v text, PRIMARY KEY (a, b));
INSERT INTO hk.p SELECT a, b, 'x' FROM generate_series(1, 20) a, generate_series(1, 20) b;
SELECT dolt_commit('-Am', 'p') IS NOT NULL AS committed;
UPDATE hk.p SET v = 'y' WHERE a = 3 AND b > 10;
SELECT dolt_commit('-am', 'p2') IS NOT NULL AS committed;
EXPLAIN (COSTS OFF) SELECT (row).v FROM dolt_history_table(NULL::hk.p) WHERE (row).a = 3 AND (row).b > 15;
SELECT same($$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.p) WHERE (row).a = 3 AND (row).b > 15$$,
			$$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.p) WHERE (row).a + 0 = 3 AND (row).b + 0 > 15$$) AS prefix_range;
SELECT same($$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.p) WHERE (row).a = 3$$,
			$$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hk.p) WHERE (row).a + 0 = 3$$) AS prefix;
-- (the second column alone bounds nothing: not the key's prefix)
EXPLAIN (COSTS OFF) SELECT (row).v FROM dolt_history_table(NULL::hk.p) WHERE (row).b = 3;

-- Values equal as the key compares them, in an IN list: each key read
-- once, as the function gives it once (5 and 5; 1.0, 1.00 and 1; citext's
-- 'abc' and 'ABC'; a parameter's array, in a generic plan; a prefix's IN
-- list with a range after it).
CREATE FUNCTION bounded_whole(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	a bigint;
	b bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM dolt_history_table(NULL::%s) h WHERE %s',
				   split_part(q, '|', 1), split_part(q, '|', 2)) INTO a;
	EXECUTE format('SELECT count(*) FROM (SELECT * FROM dolt_history_table(NULL::%s) OFFSET 0) h WHERE %s',
				   split_part(q, '|', 1), split_part(q, '|', 2)) INTO b;
	RETURN a || ' ' || b;
END $$;
SELECT bounded_whole('hk.t|(row).id IN (5, 5)') AS bounded_whole;
SELECT bounded_whole('hk.t|(row).id = ANY (ARRAY[5, 5, NULL])') AS bounded_whole;
CREATE TABLE hk.n (k numeric PRIMARY KEY, v text);
INSERT INTO hk.n VALUES (1, 'one'), (2, 'two');
SELECT dolt_commit('-Am', 'n') IS NOT NULL AS committed;
SELECT bounded_whole('hk.n|(row).k IN (1.0, 1.00, 1)') AS numeric_equal;
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS citext;
RESET client_min_messages;
CREATE TABLE hk.c (k citext PRIMARY KEY, v text);
INSERT INTO hk.c VALUES ('abc', 'x'), ('def', 'y');
SELECT dolt_commit('-Am', 'c') IS NOT NULL AS committed;
SELECT bounded_whole($$hk.c|(row).k IN ('abc'::citext, 'ABC'::citext)$$) AS citext_equal;
CREATE TABLE hk.m (a int, b text, c int, PRIMARY KEY (a, b, c));
INSERT INTO hk.m SELECT a, 'b' || b, c FROM generate_series(1, 5) a,
	generate_series(1, 3) b, generate_series(490, 520) c;
SELECT dolt_commit('-Am', 'm') IS NOT NULL AS committed;
SELECT bounded_whole($$hk.m|(row).a IN (3, 4, 3) AND (row).b = 'b2' AND (row).c > 500$$) AS prefix_in_range;
SET plan_cache_mode = force_generic_plan;
PREPARE in_array(int[]) AS SELECT count(*) FROM dolt_history_table(NULL::hk.t) WHERE (row).id = ANY ($1);
EXPLAIN (COSTS OFF) EXECUTE in_array('{5,6,5}');
EXECUTE in_array('{5,6,5}');
SELECT count(*) AS whole FROM (SELECT * FROM dolt_history_table(NULL::hk.t) OFFSET 0) h
WHERE (row).id = ANY ('{5,6,5}');
RESET plan_cache_mode;

-- The entry's row used whole, made of the scan's columns: in the target
-- list, or only in the scan's own filters; the function's rows.
EXPLAIN (COSTS OFF) SELECT h FROM dolt_history_table(NULL::hk.t) h WHERE (row).id = 5;
EXPLAIN (COSTS OFF) SELECT count(*) FROM dolt_history_table(NULL::hk.t) h
WHERE (row).id = 5 AND h IS NOT NULL;
SELECT count(h) AS whole_rows FROM dolt_history_table(NULL::hk.t) h WHERE (row).id = 5;
SELECT same($$SELECT h FROM dolt_history_table(NULL::hk.t) h WHERE (row).id = 5$$,
			$$SELECT h FROM (SELECT * FROM dolt_history_table(NULL::hk.t) OFFSET 0) h WHERE (row).id = 5$$) AS whole;
SELECT same($$SELECT row_to_json(h)::text FROM dolt_history_table(NULL::hk.t) h WHERE (row).id = 5$$,
			$$SELECT row_to_json(h)::text FROM (SELECT * FROM dolt_history_table(NULL::hk.t) OFFSET 0) h WHERE (row).id = 5$$) AS as_json;
SELECT bounded_whole('hk.t|(row).id = 5 AND h IS NOT NULL') AS is_not_null;
SELECT bounded_whole($$hk.t|(row).id = 5 AND row_to_json(h)::text LIKE '%"id":5%'$$) AS json_like;
SELECT bounded_whole('hk.t|(row).id = 5 AND num_nonnulls(h) > 0') AS nonnulls;
SELECT EXISTS (SELECT 1 FROM dolt_history_table(NULL::hk.t) h
	WHERE (row).id = 5 AND h IS NOT NULL) AS exists_whole;
SELECT count(*) AS joined FROM generate_series(5, 6) k(id)
	JOIN dolt_history_table(NULL::hk.t) h ON (h.row).id = 5 AND h IS NOT NULL;

-- The scan is dolt_history_table's: one who can't execute it can't
-- read it, though the table is granted.
CREATE ROLE regress_history_key_nobody;
GRANT USAGE ON SCHEMA hk TO regress_history_key_nobody;
GRANT SELECT ON hk.t TO regress_history_key_nobody;
REVOKE EXECUTE ON FUNCTION dolt_history_table(anyelement) FROM PUBLIC;
SET ROLE regress_history_key_nobody;
SELECT count(*) FROM dolt_history_table(NULL::hk.t) WHERE (row).id = 5;
RESET ROLE;
GRANT EXECUTE ON FUNCTION dolt_history_table(anyelement) TO PUBLIC;
REVOKE ALL ON hk.t FROM regress_history_key_nobody;
REVOKE ALL ON SCHEMA hk FROM regress_history_key_nobody;
DROP ROLE regress_history_key_nobody;

-- A keyless table: nothing to seek by.
CREATE TABLE hk.k (id int, v text);
INSERT INTO hk.k VALUES (1, 'a'), (1, 'a'), (2, 'b');
SELECT dolt_commit('-Am', 'k') IS NOT NULL AS committed;
EXPLAIN (COSTS OFF) SELECT (row).v FROM dolt_history_table(NULL::hk.k) WHERE (row).id = 1;
SELECT count(*) AS copies FROM dolt_history_table(NULL::hk.k) WHERE (row).id = 1;

\c :home
DROP DATABASE regress_history_key_seek;

-- The history horizon: the history before it severed, a bounded read
-- stops there as the function does.  (A database of its own: commits a
-- day apart, the first three severed.)
CREATE DATABASE regress_history_key_horizon;
\c regress_history_key_horizon
CREATE FUNCTION same(bounded text, whole text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, %L ORDER BY r::text) FROM (%s) r', ',', bounded) INTO a;
	EXECUTE format('SELECT string_agg(r::text, %L ORDER BY r::text) FROM (%s) r', ',', whole) INTO b;
	RETURN a IS NOT DISTINCT FROM b AND a IS NOT NULL;
END $$;
CREATE SCHEMA hz VERSIONED;
CREATE TABLE hz.t (id int PRIMARY KEY, v text);
INSERT INTO hz.t SELECT g, 'c0' FROM generate_series(1, 50) g;
CREATE FUNCTION hz_commits(n int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN
	FOR d IN 1..n LOOP
		UPDATE hz.t SET v = 'c' || d WHERE id IN (5, d);
		PERFORM dolt_commit('-Am', 'c' || d, '--date',
			(timestamptz '2020-01-01 00:00+00' + make_interval(days => d))::text);
	END LOOP;
	RETURN n;
END $$;
SELECT hz_commits(6) AS commits;
SELECT count(*) > 0 AS severed FROM dolt_history_drop('2020-01-04 00:00+00');
SELECT count(*) AS rows_of_5 FROM dolt_history_table(NULL::hz.t) WHERE (row).id = 5;
SELECT same($$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hz.t) WHERE (row).id = 5$$,
			$$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::hz.t) WHERE (row).id + 0 = 5$$) AS past_horizon;
\c :home
DROP DATABASE regress_history_key_horizon;
