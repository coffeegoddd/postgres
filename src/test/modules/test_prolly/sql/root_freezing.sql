-- A commit's roots hold its heap pages frozen as of the commit: the
-- versions the commit's snapshot sees, xmin frozen and xmax cleared;
-- every other version a dead line pointer (never an unused one, which an
-- index entry of the root's could still point at), a HOT chain's root
-- redirecting to the version left.  A reset gives the table those pages,
-- found by its indexes as they are, and vacuumed into a consistent
-- visibility map.

SELECT current_database() AS home \gset
CREATE DATABASE root_freezing;
ALTER DATABASE root_freezing SET prolly.restore_min_changes = 0;
\c root_freezing
SET client_min_messages = warning;
CREATE EXTENSION pageinspect;
CREATE EXTENSION pg_visibility;
RESET client_min_messages;

CREATE SCHEMA rf VERSIONED;
CREATE TABLE rf.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL)
	WITH (fillfactor = 60, autovacuum_enabled = off);
CREATE INDEX t_v ON rf.t (v);
INSERT INTO rf.t SELECT g, g % 10, repeat('w', 40) FROM generate_series(1, 3000) g;
SELECT dolt_commit('-Am', 'base') IS NOT NULL AS committed;

CREATE FUNCTION digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM rf.t t
$$;
-- each page's line pointers by state, and the versions not frozen
CREATE FUNCTION states() RETURNS TABLE (normal bigint, redirect bigint,
	dead bigint, unused bigint, not_frozen bigint, with_xmax bigint)
LANGUAGE sql AS $$
	SELECT count(*) FILTER (WHERE lp_flags = 1), count(*) FILTER (WHERE lp_flags = 2),
		count(*) FILTER (WHERE lp_flags = 3), count(*) FILTER (WHERE lp_flags = 0),
		count(*) FILTER (WHERE lp_flags = 1 AND (t_infomask & 768) <> 768),
		count(*) FILTER (WHERE lp_flags = 1 AND t_xmax <> 0)
	FROM generate_series(0, pg_relation_size('rf.t') / 8192 - 1) b,
		LATERAL heap_page_items(get_raw_page('rf.t', b::int))
$$;
-- an index scan finds exactly the rows a sequential scan does, by v
CREATE FUNCTION index_agrees() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	seq bigint[];
	idx bigint[];
BEGIN
	SET LOCAL enable_indexscan = off;
	SET LOCAL enable_bitmapscan = off;
	SELECT array_agg(c ORDER BY v) INTO seq
	FROM (SELECT v, count(*) c FROM rf.t GROUP BY v) s;
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_indexscan = on;
	SELECT array_agg((SELECT count(*) FROM rf.t t WHERE t.v = s.v AND t.id > 0) ORDER BY v)
		INTO idx FROM (SELECT DISTINCT v FROM rf.t) s;
	RETURN seq = idx AND
		(SELECT count(*) FROM rf.t t, generate_series(1, 10000) g
		 WHERE t.id = g AND t.id <> g) = 0;
END $$;

-- versions of every kind on the pages as c1 is committed: updated in place
-- (HOT) and of an indexed column, deleted, an aborted insert, update and
-- delete, and the committing transaction's own
UPDATE rf.t SET w = 'hot' || id WHERE id % 7 = 0;
UPDATE rf.t SET v = v + 100 WHERE id % 11 = 0;
DELETE FROM rf.t WHERE id % 13 = 0;
BEGIN;
INSERT INTO rf.t SELECT g, -1, 'aborted' FROM generate_series(5001, 5100) g;
UPDATE rf.t SET w = 'aborted' WHERE id % 17 = 0;
DELETE FROM rf.t WHERE id % 19 = 0;
ROLLBACK;
BEGIN;
UPDATE rf.t SET w = 'own' WHERE id % 23 = 0;
SELECT digest() AS at_c1 \gset
-- (line pointers a prune left unused, on the table's pages, before)
SELECT unused AS unused_c1 FROM states() \gset
SELECT dolt_commit('-am', 'c1') IS NOT NULL AS committed;
COMMIT;

-- then changed again, committed
UPDATE rf.t SET w = 'later' WHERE id <= 1500;
DELETE FROM rf.t WHERE id > 2500;
SELECT dolt_commit('-am', 'c2') IS NOT NULL AS committed;

-- back to c1: its rows, every version left frozen, none unused
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT digest() = :'at_c1' AS at_c1;
SELECT normal > 0 AS some, redirect > 0 AS redirects, dead > 0 AS some_dead,
	unused = :unused_c1 AS none_made_unused, not_frozen, with_xmax FROM states();
SELECT index_agrees();
-- each HOT-updated row found through its chain's root by the primary key
SELECT count(*) AS hot_rows FROM rf.t WHERE w LIKE 'hot%' \gset
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) = :hot_rows AS hot_by_id FROM rf.t t, generate_series(7, 3000, 7) g
WHERE t.id = g AND t.w LIKE 'hot%';
RESET enable_seqscan;
RESET enable_bitmapscan;

-- rows added after: the dead line pointers aren't reused before VACUUM
-- has removed their index entries, so no scan finds a wrong row
INSERT INTO rf.t SELECT g, g % 10, 'new' FROM generate_series(10001, 10500) g;
SELECT index_agrees();
VACUUM rf.t;
INSERT INTO rf.t SELECT g, -2, 'after vacuum' FROM generate_series(20001, 20500) g;
SELECT index_agrees();
SELECT count(*) AS found_by_v FROM rf.t WHERE v = -2;

-- the visibility map, after VACUUM, true of the pages
VACUUM (FREEZE) rf.t;
SELECT count(*) AS not_frozen FROM pg_check_frozen('rf.t');
SELECT count(*) AS not_visible FROM pg_check_visible('rf.t');

-- a page found unchanged since a root, then given a hint (which asks
-- whether it is shared, settling it), then changed: the change still
-- marks it changed since, so the next commit's root has it
CREATE TABLE rf.h (id int PRIMARY KEY, v int NOT NULL) WITH (autovacuum_enabled = off);
INSERT INTO rf.h SELECT g, 0 FROM generate_series(1, 100) g;
SELECT dolt_commit('-Am', 'h0') IS NOT NULL AS committed;
SELECT count(*) FROM rf.h;
UPDATE rf.h SET v = 1 WHERE id <= 10;
SELECT md5(string_agg(h::text, ',' ORDER BY id)) AS at_h1 FROM rf.h h \gset
SELECT dolt_commit('-am', 'h1') IS NOT NULL AS committed;
UPDATE rf.h SET v = 2;
SELECT dolt_commit('-am', 'h2') IS NOT NULL AS committed;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT md5(string_agg(h::text, ',' ORDER BY id)) = :'at_h1' AS at_h1 FROM rf.h h;

-- a relation restored from a root kept as captured (prolly.freeze_roots
-- off) is made of unfrozen pages: the next frozen root is never made of
-- that root (only a frozen one, or a copy of one, is a base), so every
-- page it holds is frozen
CREATE TABLE rf.m (id int PRIMARY KEY, w text NOT NULL) WITH (autovacuum_enabled = off);
INSERT INTO rf.m SELECT g, 'a' FROM generate_series(1, 2000) g;
SET prolly.freeze_roots = off;
SELECT dolt_commit('-Am', 'm1') IS NOT NULL AS committed;
UPDATE rf.m SET w = 'b' WHERE id <= 5;
SELECT dolt_commit('-am', 'm2') IS NOT NULL AS committed;
SELECT dolt_reset('--hard', 'HEAD~1');
RESET prolly.freeze_roots;
UPDATE rf.m SET w = 'c' WHERE id = 2000;
SELECT dolt_commit('-am', 'm3') IS NOT NULL AS committed;
UPDATE rf.m SET w = 'd' WHERE id = 1000;
SELECT dolt_commit('-am', 'm4') IS NOT NULL AS committed;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT count(*) FILTER (WHERE lp_flags = 1 AND (t_infomask & 768) <> 768) AS not_frozen,
	count(*) FILTER (WHERE lp_flags = 1) AS versions
FROM generate_series(0, pg_relation_size('rf.m') / 8192 - 1) b,
	LATERAL heap_page_items(get_raw_page('rf.m', b::int));

\c :home
DROP DATABASE root_freezing;
