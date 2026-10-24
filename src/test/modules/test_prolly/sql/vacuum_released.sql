-- A deleted branch's copy of a table is let go of in the background: until
-- then the pages it shared with the table count as shared, and a VACUUM
-- would leave them as they are or copy them to prune or freeze them,
-- though nothing else will hold them.  A VACUUM lets go of that first: it
-- prunes and freezes the pages in place, copying none.  A branch still
-- live keeps its pages: both read as they were, and an aggressive VACUUM
-- on each freezes every tuple each sees.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT current_database() AS home \gset
CREATE DATABASE vacuum_released;
-- (pages shared with a branch alone: pages kept as of each commit are shared too, which VACUUM leaves as they are)
ALTER DATABASE vacuum_released SET prolly.kept_commits = 0;
\c vacuum_released
CREATE EXTENSION test_prolly;
CREATE SCHEMA vr VERSIONED;
CREATE TABLE vr.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
INSERT INTO vr.t SELECT g, repeat('v', 100) FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'rows')::text) > 0 AS committed;

CREATE FUNCTION counted(n text) RETURNS bigint LANGUAGE sql AS $$
	SELECT value FROM pg_stat_prolly WHERE name = n
$$;

-- a branch used (its copy shares every page), then deleted: its copy is
-- queued, still holding the pages
SELECT dolt_branch('gone');
SELECT dolt_checkout('gone');
SELECT count(*) FROM vr.t;
SELECT dolt_checkout('main');
-- (dead tuples on every page, the deletes copying the pages they change)
DELETE FROM vr.t WHERE id % 20 = 0;
SELECT length(dolt_commit('-am', 'deletes')::text) > 0 AS committed;
SELECT dolt_branch('-D', 'gone');
SELECT test_bmap_release_pending() > 0 AS gone_queued;

-- a VACUUM lets go of the deleted copy first, and copies nothing
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
VACUUM (PROCESS_TOAST false) vr.t;
SELECT pg_stat_force_next_flush();
SELECT counted('page_copies_shared') AS copied_by_vacuum,
	counted('bmap_release_drained') > 0 AS released_first;
SELECT n_dead_tup FROM pg_stat_user_tables WHERE relid = 'vr.t'::regclass;
SELECT count(*), sum(id) FROM vr.t;

-- frozen in place too
SELECT pg_stat_reset_shared('prolly');
VACUUM (FREEZE, PROCESS_TOAST false) vr.t;
SELECT pg_stat_force_next_flush();
SELECT counted('page_copies_shared') AS copied_to_freeze;
SELECT age(relfrozenxid) < 5 AS frozen FROM pg_class WHERE oid = 'vr.t'::regclass;

-- A live branch, used, then the table changed on main: each branch's
-- aggressive VACUUM freezes what it sees (copying shared pages it must
-- freeze), and each reads as it did.
SELECT dolt_branch('live');
SELECT dolt_checkout('live');
SELECT count(*) FROM vr.t;
SELECT dolt_checkout('main');
UPDATE vr.t SET v = 'main' WHERE id % 1000 = 1;
SELECT length(dolt_commit('-am', 'main changes')::text) > 0 AS committed;
SET vacuum_freeze_table_age = 0;
VACUUM (PROCESS_TOAST false) vr.t;
SELECT age(relfrozenxid) < 5 AS main_frozen FROM pg_class WHERE oid = 'vr.t'::regclass;
SELECT count(*), count(*) FILTER (WHERE v = 'main') FROM vr.t;
SELECT dolt_checkout('live');
VACUUM (PROCESS_TOAST false) vr.t;
SELECT age(relfrozenxid) < 5 AS live_frozen FROM pg_class WHERE oid = 'vr.t'::regclass;
SELECT count(*), count(*) FILTER (WHERE v = 'main') FROM vr.t;
SELECT dolt_checkout('main');
RESET vacuum_freeze_table_age;
SELECT count(*), count(*) FILTER (WHERE v = 'main') FROM vr.t;

\c :home
DROP DATABASE vacuum_released;
