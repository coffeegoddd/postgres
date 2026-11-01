-- What the catalog filter costs: each object's branch is read from the
-- catalogs once, and again only after the object changes.  Neither a
-- branch change nor what other branches hold makes a session read again.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SELECT pg_branch_id_allocate() AS b1 \gset
SELECT pg_branch_id_allocate() AS b2 \gset

-- the same versioned schema on two branches, a hundred tables each
SET catalog_branch = :b1;
CREATE SCHEMA cost VERSIONED;
SELECT format('CREATE TABLE cost.t%s (a int, b text)', g)
FROM generate_series(1, 100) g \gexec
SET catalog_branch = :b2;
CREATE SCHEMA cost VERSIONED;
SELECT format('CREATE TABLE cost.t%s (a int, b text)', g)
FROM generate_series(1, 100) g \gexec

-- the first scans learn every object they meet
SET catalog_branch = :b1;
SELECT count(*) FROM pg_attribute WHERE attname = 'b' AND attrelid IN
	(SELECT oid FROM pg_class WHERE relnamespace = 'cost'::regnamespace);
SELECT reads AS r0 FROM test_branch_map_stats() \gset

-- asked again, nothing is read
SELECT count(*) FROM pg_attribute WHERE attname = 'b' AND attrelid IN
	(SELECT oid FROM pg_class WHERE relnamespace = 'cost'::regnamespace);
SELECT reads - :r0 AS reads_again FROM test_branch_map_stats();

-- nor after a branch change
SET catalog_branch = :b2;
SELECT count(*) FROM pg_attribute WHERE attname = 'b' AND attrelid IN
	(SELECT oid FROM pg_class WHERE relnamespace = 'cost'::regnamespace);
SELECT reads - :r0 AS reads_after_branch_change FROM test_branch_map_stats();

-- DDL on one branch costs only the objects it changed: a new table's
-- relation, row and array types, TOAST table and index (learned by the
-- check of its dependencies and by the next scan; 17 reads in all), not
-- the two hundred tables already known
SELECT reads AS r1 FROM test_branch_map_stats() \gset
CREATE TABLE cost.extra (a int, b text);
SET catalog_branch = :b1;
SELECT count(*) FROM pg_attribute WHERE attname = 'b' AND attrelid IN
	(SELECT oid FROM pg_class WHERE relnamespace = 'cost'::regnamespace);
SELECT reads - :r1 BETWEEN 1 AND 20 AS reads_what_changed FROM test_branch_map_stats();

-- a dropped table's answers are forgotten
SELECT forgotten AS f0 FROM test_branch_map_stats() \gset
DROP TABLE cost.t1;
SELECT forgotten > :f0 AS forgot_dropped FROM test_branch_map_stats();

-- a new session's full scan of pg_attribute meets every branch's tables,
-- their TOAST tables and indexes, and learns them all from one scan of
-- pg_class and one of pg_index once 64 were read one by one: 108 reads,
-- where one of each made 1002
\c
SET catalog_branch = :b1;
SET max_parallel_workers_per_gather = 0;
SELECT count(*) > 0 AS columns FROM pg_attribute;
SELECT reads < 200 AS learned_at_once FROM test_branch_map_stats();

SET client_min_messages = warning;
DROP SCHEMA cost CASCADE;
SET catalog_branch = :b2;
DROP SCHEMA cost CASCADE;
RESET catalog_branch;
RESET client_min_messages;
