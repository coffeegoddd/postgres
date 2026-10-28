-- dolt_at(ref, NULL::t) with quals on the table's primary key: the
-- commit's data tree is sought for the keys the quals bound, rather than
-- every row read, so a point read is O(the tree's depth), whatever the
-- table's size.  A value of another type than the key's that casts to it
-- implicitly bounds it too (an int literal against a bigint key), for
-- dolt_history_table() as well.  The rows are the function's own: the
-- same as a qual the scan can't bound (id + 0 = 5) gives, through NULLs,
-- collations, a key of two columns, WORKING (read whole) and a keyless
-- table (never bounded).

SELECT current_database() AS home \gset
\set VERBOSITY terse
CREATE DATABASE regress_at_key_seek;
\c regress_at_key_seek

-- (the tree nodes a statement reads, its counts flushed)
CREATE FUNCTION nodes_read() RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = 'nodes_read'), 0)
$$;
\set flush 'SELECT pg_stat_force_next_flush() IS NULL AS flushed \\gset'
CREATE FUNCTION same(bounded text, whole text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, %L ORDER BY r::text) FROM (%s) r', ',', bounded) INTO a;
	EXECUTE format('SELECT string_agg(r::text, %L ORDER BY r::text) FROM (%s) r', ',', whole) INTO b;
	RETURN a IS NOT DISTINCT FROM b AND a IS NOT NULL;
END $$;
-- (the count a qual on dolt_at(ref, NULL::t) gives, bounded, then read whole)
CREATE FUNCTION bounded_whole(t text, ref text, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	a bigint;
	b bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM dolt_at(%L, NULL::%s) h WHERE %s', ref, t, q) INTO a;
	EXECUTE format('SELECT count(*) FROM (SELECT * FROM dolt_at(%L, NULL::%s) OFFSET 0) h WHERE %s',
				   ref, t, q) INTO b;
	RETURN a || ' ' || b;
END $$;

CREATE SCHEMA ak VERSIONED;
CREATE TABLE ak.small (id bigint PRIMARY KEY, v text NOT NULL);
INSERT INTO ak.small SELECT g, repeat('v', 100) || g FROM generate_series(1, 500) g;
CREATE TABLE ak.big (id bigint PRIMARY KEY, v text NOT NULL);
INSERT INTO ak.big SELECT g, repeat('v', 100) || g FROM generate_series(1, 32000) g;
SELECT dolt_commit('-Am', 'load') IS NOT NULL AS committed;
UPDATE ak.big SET v = 'changed' WHERE id IN (5, 31999);
SELECT dolt_commit('-am', 'changed') IS NOT NULL AS committed;

-- the plan: the quals bound the key, shown, and stay the filter; an int
-- literal against the bigint key is cast for the bound
EXPLAIN (COSTS OFF) SELECT v FROM dolt_at('HEAD~1', NULL::ak.big) WHERE id = 5;
EXPLAIN (COSTS OFF) SELECT v FROM dolt_at('HEAD~1', NULL::ak.big) WHERE id IN (1, 16000, 32000);
EXPLAIN (COSTS OFF) SELECT v FROM dolt_at('HEAD~1', NULL::ak.big) WHERE id > 31990 AND id <= 31995;
EXPLAIN (COSTS OFF) SELECT v FROM dolt_at('HEAD~1', NULL::ak.big) WHERE id = 5::bigint;
EXPLAIN (COSTS OFF) SELECT (row).v FROM dolt_history_table(NULL::ak.big) WHERE (row).id = 5;
-- (a qual not on the key's column itself isn't a bound; nor a value of a
-- type that doesn't cast to the key's implicitly)
EXPLAIN (COSTS OFF) SELECT v FROM dolt_at('HEAD~1', NULL::ak.big) WHERE id + 0 = 5;
EXPLAIN (COSTS OFF) SELECT v FROM dolt_at('HEAD~1', NULL::ak.big) WHERE id = 5.0;

-- The reads: a point read seeks the tree, a few nodes, the same few at 500
-- rows and at 32000; the whole read reads every node, growing with the rows.
:flush
SELECT nodes_read() AS before \gset
SELECT right(v, 6) AS v FROM dolt_at('HEAD~1', NULL::ak.small) WHERE id = 5;
:flush
SELECT nodes_read() - :before AS point_small \gset
SELECT nodes_read() AS before \gset
SELECT right(v, 6) AS v FROM dolt_at('HEAD~1', NULL::ak.big) WHERE id = 5;
:flush
SELECT nodes_read() - :before AS point_big \gset
SELECT nodes_read() AS before \gset
SELECT id, right(v, 6) AS v FROM dolt_at('HEAD~1', NULL::ak.big) WHERE id IN (1, 16000, 32000) ORDER BY id;
:flush
SELECT nodes_read() - :before AS three_big \gset
SELECT nodes_read() AS before \gset
SELECT count(*) FROM dolt_at('HEAD~1', NULL::ak.big) WHERE id + 0 = 5;
:flush
SELECT nodes_read() - :before AS whole_big \gset
SELECT nodes_read() AS before \gset
SELECT count(*) FROM dolt_history_table(NULL::ak.big) WHERE (row).id = 5;
:flush
SELECT nodes_read() - :before AS history_point \gset
SELECT nodes_read() AS before \gset
SELECT count(*) FROM dolt_history_table(NULL::ak.big) WHERE (row).id + 0 = 5;
:flush
SELECT nodes_read() - :before AS history_whole \gset
SELECT :point_big <= :point_small + 8 AS point_flat_in_rows,
	:point_big <= 40 AS point_bounded,
	:three_big <= 3 * 40 AS three_bounded,
	:whole_big > 10 * :point_big AS whole_reads_more,
	:history_point <= 2 * 40 AS history_point_bounded,
	:history_whole > 10 * :history_point AS history_whole_reads_more;

-- the same rows as the function gives
SELECT same($$SELECT h FROM dolt_at('HEAD~1', NULL::ak.big) h WHERE id = 5$$,
			$$SELECT h FROM dolt_at('HEAD~1', NULL::ak.big) h WHERE id + 0 = 5$$) AS eq;
SELECT same($$SELECT h FROM dolt_at('HEAD', NULL::ak.big) h WHERE id = 5$$,
			$$SELECT h FROM dolt_at('HEAD', NULL::ak.big) h WHERE id + 0 = 5$$) AS eq_head;
SELECT same($$SELECT h FROM dolt_at('HEAD~1', NULL::ak.big) h WHERE id IN (5, 700, 99999)$$,
			$$SELECT h FROM dolt_at('HEAD~1', NULL::ak.big) h WHERE id + 0 IN (5, 700, 99999)$$) AS in_list;
SELECT same($$SELECT h FROM dolt_at('HEAD~1', NULL::ak.big) h WHERE id > 31990 AND id <= 31995$$,
			$$SELECT h FROM dolt_at('HEAD~1', NULL::ak.big) h WHERE id + 0 > 31990 AND id + 0 <= 31995$$) AS range;
SELECT same($$SELECT h FROM dolt_at('HEAD~1', NULL::ak.big) h WHERE id < 3$$,
			$$SELECT h FROM dolt_at('HEAD~1', NULL::ak.big) h WHERE id + 0 < 3$$) AS below;
SELECT same($$SELECT h FROM dolt_at('HEAD~1', NULL::ak.big) h WHERE id >= 31999$$,
			$$SELECT h FROM dolt_at('HEAD~1', NULL::ak.big) h WHERE id + 0 >= 31999$$) AS above;
SELECT bounded_whole('ak.big', 'HEAD~1', 'id = 5::smallint') AS from_int2;
SELECT bounded_whole('ak.big', 'HEAD~1', 'id IN (5, 5, NULL)') AS in_dup_null;
SELECT bounded_whole('ak.big', 'HEAD~1', 'id = ANY (ARRAY[5, 6]::int[])') AS int_array;
SELECT bounded_whole('ak.big', 'HEAD~1', 'id = NULL::int') AS null_value;
SELECT bounded_whole('ak.big', 'HEAD~1', 'id = 99999') AS no_such_key;
SELECT same($$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::ak.big) WHERE (row).id = 31999$$,
			$$SELECT (row).*, commit_hash FROM dolt_history_table(NULL::ak.big) WHERE (row).id + 0 = 31999$$) AS history_cast;
-- (a parameter, NULL, an outer row's value, and a generic plan's array)
PREPARE at_key(int) AS SELECT right(v, 6) AS v FROM dolt_at('HEAD~1', NULL::ak.big) WHERE id = $1;
EXECUTE at_key(5);
EXECUTE at_key(NULL);
SELECT k.id, (SELECT v FROM dolt_at('HEAD', NULL::ak.big) h WHERE h.id = k.id) AS v
FROM (VALUES (5::bigint), (31999), (99999)) k(id) ORDER BY k.id;
SET plan_cache_mode = force_generic_plan;
PREPARE at_refs(text, bigint[]) AS SELECT count(*) FROM dolt_at($1, NULL::ak.big) WHERE id = ANY ($2);
EXPLAIN (COSTS OFF) EXECUTE at_refs('HEAD~1', '{5,6,5}');
EXECUTE at_refs('HEAD~1', '{5,6,5}');
RESET plan_cache_mode;
-- (a NULL ref is refused as the function refuses it)
SELECT v FROM dolt_at(NULL, NULL::ak.big) WHERE id = 5;

-- A primary key of two columns, text in its own collation: its first,
-- then a range on its second; equal as the key compares them.
CREATE TABLE ak.p (a text COLLATE "C", b int, v text, PRIMARY KEY (a, b));
INSERT INTO ak.p SELECT 'k' || a, b, 'x' FROM generate_series(1, 20) a, generate_series(1, 20) b;
INSERT INTO ak.p VALUES ('K3', 1, 'upper');
SELECT dolt_commit('-Am', 'p') IS NOT NULL AS committed;
EXPLAIN (COSTS OFF) SELECT v FROM dolt_at('HEAD', NULL::ak.p) WHERE a = 'k3' AND b > 15;
SELECT bounded_whole('ak.p', 'HEAD', $$a = 'k3' AND b > 15$$) AS prefix_range;
SELECT bounded_whole('ak.p', 'HEAD', $$a = 'k3'$$) AS prefix;
SELECT bounded_whole('ak.p', 'HEAD', $$a IN ('k3', 'K3', 'k3')$$) AS prefix_in;
-- (another collation's comparison isn't the key's order: not a bound)
EXPLAIN (COSTS OFF) SELECT v FROM dolt_at('HEAD', NULL::ak.p) WHERE a = 'k3' COLLATE "POSIX";
SELECT bounded_whole('ak.p', 'HEAD', $$a > 'k19' COLLATE "POSIX"$$) AS other_collation;
-- (the second column alone bounds nothing: not the key's prefix)
EXPLAIN (COSTS OFF) SELECT v FROM dolt_at('HEAD', NULL::ak.p) WHERE b = 3;

-- (a value whose cast to the key's type truncates isn't a bound: text
-- against a name key, the 64th byte cut)
CREATE TABLE ak.nm (k name PRIMARY KEY);
INSERT INTO ak.nm VALUES (repeat('a', 63)), ('b');
SELECT dolt_commit('-Am', 'nm') IS NOT NULL AS committed;
EXPLAIN (COSTS OFF) SELECT k FROM dolt_at('HEAD', NULL::ak.nm) WHERE k < repeat('a', 64)::text;
SELECT bounded_whole('ak.nm', 'HEAD', $$k < repeat('a', 64)::text$$) AS truncated;

-- WORKING: read whole, the rows the same.
UPDATE ak.big SET v = 'working' WHERE id = 5;
SELECT v FROM dolt_at('WORKING', NULL::ak.big) WHERE id = 5;
SELECT bounded_whole('ak.big', 'WORKING', 'id IN (5, 6)') AS working;

-- A keyless table: nothing to seek by.
CREATE TABLE ak.k (id int, v text);
INSERT INTO ak.k VALUES (1, 'a'), (1, 'a'), (2, 'b');
SELECT dolt_commit('-Am', 'k') IS NOT NULL AS committed;
EXPLAIN (COSTS OFF) SELECT v FROM dolt_at('HEAD', NULL::ak.k) WHERE id = 1;
SELECT count(*) AS copies FROM dolt_at('HEAD', NULL::ak.k) WHERE id = 1;

-- The scan is dolt_at's: one who can't execute it can't read it, though
-- the table is granted.
CREATE ROLE regress_at_key_nobody;
GRANT USAGE ON SCHEMA ak TO regress_at_key_nobody;
GRANT SELECT ON ak.big TO regress_at_key_nobody;
REVOKE EXECUTE ON FUNCTION dolt_at(text, anyelement) FROM PUBLIC;
SET ROLE regress_at_key_nobody;
SELECT v FROM dolt_at('HEAD', NULL::ak.big) WHERE id = 5;
RESET ROLE;
GRANT EXECUTE ON FUNCTION dolt_at(text, anyelement) TO PUBLIC;
REVOKE ALL ON ak.big FROM regress_at_key_nobody;
REVOKE ALL ON SCHEMA ak FROM regress_at_key_nobody;
DROP ROLE regress_at_key_nobody;

\c :home
DROP DATABASE regress_at_key_seek;
