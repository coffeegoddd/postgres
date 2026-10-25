-- A commit whose kept pages are gone is no longer kept: a restore of
-- another commit gives each table it restores new storage, and the
-- storage replaced, which every other commit's snapshot of the table was
-- in, goes with it, as it does after a TRUNCATE or a rewrite.  Such a
-- commit is dropped from the branch's kept commits by the same
-- transaction, so it no longer counts toward prolly.kept_commits, keeps
-- its commit and baseline from collection, or is offered to a move; one
-- with a snapshot left but no table restorable is retired.  A commit some
-- table of which can still be given its pages stays kept, whatever else
-- happens to the table, and one made gone by a change rolled back is kept
-- as it was, its snapshots too.

SELECT current_database() AS home \gset
CREATE DATABASE kept_dead;
ALTER DATABASE kept_dead SET prolly.restore_min_changes = 0;
ALTER DATABASE kept_dead SET prolly.kept_commits = 3;
\c kept_dead
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

CREATE FUNCTION restored() RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce((SELECT value FROM pg_stat_prolly
					 WHERE name = 'relations_restored'), 0)
$$;
-- the commits kept, as names given below
CREATE TABLE names (id text PRIMARY KEY, name text);
CREATE FUNCTION kept() RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(coalesce(n.name, '?'), ' ' ORDER BY k.ord)
	FROM test_kept_commits('main') WITH ORDINALITY k(id, ord)
	LEFT JOIN names n ON n.id = k.id
$$;
CREATE FUNCTION named(name text) RETURNS text LANGUAGE sql AS $$
	SELECT id FROM names n WHERE n.name = named.name
$$;

CREATE SCHEMA kd VERSIONED;
CREATE TABLE kd.t (id int PRIMARY KEY, v int NOT NULL);
CREATE INDEX t_v ON kd.t (v);
INSERT INTO kd.t SELECT g, 0 FROM generate_series(1, 2000) g;
INSERT INTO names SELECT (dolt_commit('-Am', 'a'))[1], 'a';
UPDATE kd.t SET v = 1 WHERE id <= 100;
INSERT INTO names SELECT (dolt_commit('-am', 'b'))[1], 'b';
UPDATE kd.t SET v = 2 WHERE id <= 100;
INSERT INTO names SELECT (dolt_commit('-am', 'c'))[1], 'c';
SELECT kept();

-- a hard reset to b restores kd.t: c's and a's snapshots were in the
-- storage it replaced, so they are kept no more, nor kept from collection
SELECT pg_stat_force_next_flush();
SELECT restored() AS before \gset
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT pg_stat_force_next_flush();
SELECT restored() > :before AS restored;
SELECT kept();
SET client_min_messages = error;
SELECT kept > 0 AS kept FROM test_chunk_gc(true);
RESET client_min_messages;
SELECT test_chunk_has(named('c')) AS c_stored;
SELECT count(*) FILTER (WHERE v = 1) AS at_b FROM kd.t;

-- so the next commits keep b, the one restorable, with them
UPDATE kd.t SET v = 3 WHERE id <= 100;
INSERT INTO names SELECT (dolt_commit('-am', 'd'))[1], 'd';
SELECT kept();

-- and a reset back to b is still O(1): its pages given back
SELECT restored() AS before \gset
SELECT dolt_reset('--hard', named('b'));
SELECT pg_stat_force_next_flush();
SELECT restored() > :before AS restored, kept();

-- a commit whose table TRUNCATE gave new storage is retired (its change
-- log still holds a snapshot, let go of by a later keep), but one with a
-- table it left still kept
CREATE TABLE kd.u (id int PRIMARY KEY);
INSERT INTO kd.u SELECT generate_series(1, 10);
INSERT INTO names SELECT (dolt_commit('-Am', 'e'))[1], 'e';
SELECT kept();
TRUNCATE kd.t;
INSERT INTO kd.t VALUES (1, 1);
INSERT INTO names SELECT (dolt_commit('-am', 'f'))[1], 'f';
SELECT kept();
TRUNCATE kd.u;
INSERT INTO names SELECT (dolt_commit('-am', 'g'))[1], 'g';
SELECT kept();

-- what leaves a table on its storage leaves its commits kept
INSERT INTO names SELECT (dolt_commit('--allow-empty', '-m', 'h'))[1], 'h';
SELECT kept();
VACUUM kd.t;
VACUUM (FREEZE) kd.u;
ANALYZE kd.t;
ALTER TABLE kd.t ADD COLUMN w int;
INSERT INTO names SELECT (dolt_commit('-am', 'i'))[1], 'i';
SELECT kept();
CREATE INDEX t_w ON kd.t (w);
INSERT INTO names SELECT (dolt_commit('-am', 'j'))[1], 'j';
SELECT kept();

-- a TRUNCATE rolled back to a savepoint, with a commit after it: kept as
-- it was
BEGIN;
SAVEPOINT s;
TRUNCATE kd.t, kd.u;
INSERT INTO names SELECT (dolt_commit('-am', 'k'))[1], 'k';
ROLLBACK TO s;
COMMIT;
SELECT kept();

-- a commit made gone by a change that rolls back still has every
-- snapshot it had, though a commit let go of shared one with it: kr.t's
-- relations, unchanged since m, are kept by m's snapshots for n and o too
SET prolly.kept_commits = 2;
CREATE SCHEMA kr VERSIONED;
CREATE TABLE kr.t (id int PRIMARY KEY, v int NOT NULL);
INSERT INTO kr.t SELECT g, 0 FROM generate_series(1, 2000) g;
CREATE TABLE kr.u (id int PRIMARY KEY, v int);
INSERT INTO kr.u SELECT g, 0 FROM generate_series(1, 2000) g;
INSERT INTO names SELECT (dolt_commit('-Am', 'm'))[1], 'm';
UPDATE kr.u SET v = 1 WHERE id <= 100;
INSERT INTO names SELECT (dolt_commit('-am', 'n'))[1], 'n';
UPDATE kr.u SET v = 2 WHERE id <= 100;
INSERT INTO names SELECT (dolt_commit('-am', 'o'))[1], 'o';
SELECT kept();
CREATE TABLE kr.w (id int PRIMARY KEY);
BEGIN;
DROP TABLE kd.t, kd.u, kr.t, kr.u;
SELECT dolt_commit('-Am', 'gone') IS NOT NULL AS committed;
ROLLBACK;
SELECT kept();
UPDATE kr.u SET v = 3 WHERE id <= 100;
UPDATE kr.t SET v = 3 WHERE id <= 100;
SELECT pg_stat_force_next_flush();
SELECT restored() AS before \gset
SELECT dolt_reset('--hard', named('n'));
SELECT pg_stat_force_next_flush();
-- (kr.t's and kr.u's: each heap, index, change log and its index)
SELECT restored() - :before AS restored;
SELECT count(*) FILTER (WHERE v = 1) AS at_n FROM kr.u;
SELECT count(*) FILTER (WHERE v = 0) AS at_n FROM kr.t;
RESET prolly.kept_commits;

\c :home
DROP DATABASE kept_dead;
