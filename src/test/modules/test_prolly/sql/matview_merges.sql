-- A materialized view's rows in a three-way merge: a REFRESH is a change
-- of them, so the side that refreshed it gives them, as a table's rows
-- come from the side that changed them; refreshed on both sides
-- differently, it is refreshed again, from the merged rows.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA mm VERSIONED;
CREATE TABLE mm.t (pk int PRIMARY KEY);
CREATE TABLE mm.o (pk int PRIMARY KEY);
CREATE MATERIALIZED VIEW mm.mv AS SELECT coalesce(sum(pk), 0) AS c FROM mm.t;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('mm_other');

-- Theirs refreshed it, ours didn't: theirs' rows.
INSERT INTO mm.o VALUES (1);
SELECT length(dolt_commit('-am', 'ours: o')::text) AS committed;
SELECT dolt_checkout('mm_other');
INSERT INTO mm.t VALUES (1);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'theirs: t, refreshed')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('mm_other', '-m', 'theirs refreshed'))[3:4];
SELECT c AS theirs_refresh FROM mm.mv;
SELECT count(*) AS clean FROM dolt_status;
-- and a fast-forward of theirs to the merge keeps it
SELECT dolt_checkout('mm_other');
SELECT (dolt_merge('main'))[2:4];
SELECT c AS still_theirs_refresh FROM mm.mv;
SELECT dolt_checkout('main');

-- Ours refreshed it, theirs didn't: ours' rows.
INSERT INTO mm.t VALUES (2);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'ours: t, refreshed')::text) AS committed;
SELECT dolt_checkout('mm_other');
INSERT INTO mm.o VALUES (2);
SELECT length(dolt_commit('-am', 'theirs: o')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('mm_other', '-m', 'ours refreshed'))[3:4];
SELECT c AS ours_refresh FROM mm.mv;

-- Both refreshed it, to other rows: refreshed from the merged rows.
SELECT dolt_checkout('mm_other');
SELECT dolt_reset('--hard', 'main');
INSERT INTO mm.t VALUES (3);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'theirs: t, refreshed')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO mm.t VALUES (4), (5);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'ours: t, refreshed')::text) AS committed;
SELECT (dolt_merge('mm_other', '-m', 'both refreshed'))[3:4];
SELECT c AS refreshed_from_merged, (SELECT sum(pk) FROM mm.t) AS t_sum FROM mm.mv;
SELECT count(*) AS clean FROM dolt_status;

-- Both refreshed it alike: those rows.
SELECT dolt_checkout('mm_other');
SELECT dolt_reset('--hard', 'main');
DELETE FROM mm.t WHERE pk = 5;
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'theirs: a row of t gone, refreshed')::text) AS committed;
SELECT dolt_checkout('main');
DELETE FROM mm.t WHERE pk IN (1, 4);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'ours: others gone, refreshed alike')::text) AS committed;
SELECT (dolt_merge('mm_other', '-m', 'refreshed alike'))[3:4];
SELECT c AS refreshed_alike, (SELECT sum(pk) FROM mm.t) AS t_sum FROM mm.mv;

-- Definitions change in the merge (ours adds a table): theirs' refresh
-- still comes.
SELECT dolt_checkout('mm_other');
SELECT dolt_reset('--hard', 'main');
INSERT INTO mm.t VALUES (6);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'theirs: t, refreshed')::text) AS committed;
SELECT dolt_checkout('main');
CREATE TABLE mm.n (pk int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'ours: a table')::text) AS committed;
SELECT (dolt_merge('mm_other', '-m', 'theirs refreshed, ours defined'))[3:4];
SELECT c AS theirs_refresh, (SELECT sum(pk) FROM mm.t) AS t_sum FROM mm.mv;

-- and both refreshed it, with definitions changing (theirs adds a
-- table): refreshed from the merged rows.
SELECT dolt_checkout('mm_other');
SELECT dolt_reset('--hard', 'main');
CREATE TABLE mm.n2 (pk int PRIMARY KEY);
INSERT INTO mm.t VALUES (20);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-Am', 'theirs: a table, t, refreshed')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO mm.t VALUES (21);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'ours: t, refreshed')::text) AS committed;
SELECT (dolt_merge('mm_other', '-m', 'both refreshed, theirs defined'))[3:4];
SELECT c AS refreshed_from_merged, (SELECT sum(pk) FROM mm.t) AS t_sum FROM mm.mv;
SELECT count(*) AS clean FROM dolt_status;
DELETE FROM mm.t WHERE pk IN (20, 21);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'back to 11')::text) AS committed;

-- Ours redefines another materialized view, so the merge makes them all
-- again: each still has the rows its side's refresh gave it, not those
-- its query reads now.
CREATE MATERIALIZED VIEW mm.mv2 AS SELECT count(*) AS c FROM mm.o;
SELECT length(dolt_commit('-Am', 'a second view')::text) AS committed;
SELECT dolt_checkout('mm_other');
SELECT dolt_reset('--hard', 'main');
INSERT INTO mm.t VALUES (7);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'theirs: t, refreshed')::text) AS committed;
SELECT dolt_checkout('main');
DROP MATERIALIZED VIEW mm.mv2;
CREATE MATERIALIZED VIEW mm.mv2 AS SELECT count(*) + 100 AS c FROM mm.o;
INSERT INTO mm.t VALUES (8);
SELECT length(dolt_commit('-Am', 'ours: t, the second view redefined')::text) AS committed;
SELECT c AS theirs_mv_before FROM mm.mv;
SELECT (dolt_merge('mm_other', '-m', 'theirs refreshed, ours redefined'))[3:4];
SELECT c AS theirs_refresh, (SELECT sum(pk) FROM mm.t) AS t_sum FROM mm.mv;
SELECT c AS ours_mv2 FROM mm.mv2;
SELECT count(*) AS clean FROM dolt_status;

-- Into uncommitted rows, a merge that would refresh it is refused (they
-- would be among the rows it reads); one taking theirs' refresh, a
-- fast-forward's too, isn't, and leaves them uncommitted.
SELECT dolt_checkout('mm_other');
SELECT dolt_reset('--hard', 'main');
INSERT INTO mm.t VALUES (9);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'theirs: t, refreshed')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO mm.t VALUES (10);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'ours: t, refreshed')::text) AS committed;
INSERT INTO mm.o VALUES (10);
SELECT (dolt_merge('mm_other'))[2:4];
SELECT dolt_reset('--hard', 'HEAD~1');
-- (a fast-forward over them)
INSERT INTO mm.o VALUES (10);
SELECT (dolt_merge('mm_other'))[2:4];
SELECT c AS theirs_refresh, (SELECT sum(pk) FROM mm.t) AS t_sum FROM mm.mv;
SELECT table_name, status FROM dolt_status ORDER BY table_name;
SELECT dolt_reset('--hard');
-- (a three-way merge over them)
SELECT dolt_checkout('mm_other');
INSERT INTO mm.t VALUES (11);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 'theirs: t, refreshed')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO mm.n VALUES (1);
SELECT length(dolt_commit('-am', 'ours: n')::text) AS committed;
INSERT INTO mm.o VALUES (10);
SELECT (dolt_merge('mm_other', '-m', 'over uncommitted rows'))[3:4];
SELECT c AS theirs_refresh, (SELECT sum(pk) FROM mm.t) AS t_sum FROM mm.mv;
SELECT table_name, status FROM dolt_status ORDER BY table_name;
SELECT dolt_reset('--hard');

-- A series over uncommitted rows its views don't read: each pick merges
-- into the views as the one before left them, so taking each one's
-- refresh in turn is no refresh of both.
INSERT INTO mm.t VALUES (12);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 't 12, refreshed')::text) AS committed;
INSERT INTO mm.t VALUES (13);
REFRESH MATERIALIZED VIEW mm.mv;
SELECT length(dolt_commit('-am', 't 13, refreshed')::text) AS committed;
SELECT c AS before_revert FROM mm.mv;
INSERT INTO mm.n VALUES (2);
SELECT (dolt_revert('HEAD', 'HEAD~1'))[2:4];
SELECT c AS reverted, (SELECT sum(pk) FROM mm.t) AS t_sum FROM mm.mv;
SELECT table_name, status FROM dolt_status ORDER BY table_name;
SELECT dolt_reset('--hard');

-- A view reading another, both refreshed on both sides: the one read is
-- refreshed first, so the other reads its merged rows.
CREATE MATERIALIZED VIEW mm.b AS SELECT coalesce(sum(pk), 0) AS c FROM mm.t;
CREATE MATERIALIZED VIEW mm.a AS SELECT c * 10 AS c FROM mm.b;
SELECT length(dolt_commit('-Am', 'a view reading a view')::text) AS committed;
SELECT dolt_checkout('mm_other');
SELECT dolt_reset('--hard', 'main');
INSERT INTO mm.t VALUES (100);
REFRESH MATERIALIZED VIEW mm.b;
REFRESH MATERIALIZED VIEW mm.a;
SELECT length(dolt_commit('-am', 'theirs: t, both refreshed')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO mm.t VALUES (200);
REFRESH MATERIALIZED VIEW mm.b;
REFRESH MATERIALIZED VIEW mm.a;
SELECT length(dolt_commit('-am', 'ours: t, both refreshed')::text) AS committed;
SELECT (dolt_merge('mm_other', '-m', 'both refreshed both'))[3:4];
SELECT (SELECT c FROM mm.b) AS b, (SELECT c FROM mm.a) AS a,
	(SELECT sum(pk) FROM mm.t) AS t_sum;
SELECT count(*) AS clean FROM dolt_status;

-- and reading it through a function: one with a BEGIN ATOMIC body
-- records what it reads; one with a string body doesn't, so the views are
-- refreshed until any order of them is right.
CREATE MATERIALIZED VIEW mm.d AS SELECT coalesce(sum(pk), 0)::int AS c FROM mm.t;
CREATE FUNCTION mm.dsum() RETURNS int LANGUAGE sql STABLE
	BEGIN ATOMIC SELECT c FROM mm.d; END;
CREATE MATERIALIZED VIEW mm.c AS SELECT mm.dsum() * 10 AS c;
CREATE MATERIALIZED VIEW mm.f AS SELECT coalesce(sum(pk), 0)::int AS c FROM mm.t;
CREATE FUNCTION mm.fsum() RETURNS int LANGUAGE sql STABLE
	AS 'SELECT c FROM mm.f';
CREATE MATERIALIZED VIEW mm.e AS SELECT mm.fsum() * 10 AS c;
SELECT length(dolt_commit('-Am', 'views reading views through functions')::text) AS committed;
SELECT dolt_checkout('-b', 'mm_fn');
INSERT INTO mm.t VALUES (1000);
REFRESH MATERIALIZED VIEW mm.d;
REFRESH MATERIALIZED VIEW mm.c;
REFRESH MATERIALIZED VIEW mm.f;
REFRESH MATERIALIZED VIEW mm.e;
SELECT length(dolt_commit('-am', 'theirs: t, all refreshed')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO mm.t VALUES (2000);
REFRESH MATERIALIZED VIEW mm.d;
REFRESH MATERIALIZED VIEW mm.c;
REFRESH MATERIALIZED VIEW mm.f;
REFRESH MATERIALIZED VIEW mm.e;
SELECT length(dolt_commit('-am', 'ours: t, all refreshed')::text) AS committed;
SELECT (dolt_merge('mm_fn', '-m', 'both refreshed all'))[3:4];
SELECT (SELECT c FROM mm.d) AS d, (SELECT c FROM mm.c) AS c,
	(SELECT c FROM mm.f) AS f, (SELECT c FROM mm.e) AS e,
	(SELECT sum(pk) FROM mm.t) AS t_sum;
SELECT count(*) AS clean FROM dolt_status;

-- and through an operator, its function reading the other view (a string
-- body: refreshed again after the pass over them all).
CREATE SCHEMA mmc;
CREATE MATERIALIZED VIEW mm.ob AS SELECT coalesce(sum(pk), 0)::int AS c FROM mm.t;
CREATE FUNCTION mmc.times_ob(int, int) RETURNS int LANGUAGE plpgsql STABLE
	AS $$BEGIN RETURN $1 * (SELECT c FROM mm.ob); END$$;
CREATE OPERATOR mmc.*** (LEFTARG = int, RIGHTARG = int, FUNCTION = mmc.times_ob);
CREATE MATERIALIZED VIEW mm.oa AS SELECT 10 OPERATOR(mmc.***) 0 AS c;
SELECT length(dolt_commit('-Am', 'a view reading a view through an operator')::text) AS committed;
SELECT dolt_checkout('-b', 'mm_op');
INSERT INTO mm.t VALUES (10000);
REFRESH MATERIALIZED VIEW mm.ob;
REFRESH MATERIALIZED VIEW mm.oa;
SELECT length(dolt_commit('-am', 'theirs: t, both refreshed')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO mm.t VALUES (20000);
REFRESH MATERIALIZED VIEW mm.ob;
REFRESH MATERIALIZED VIEW mm.oa;
SELECT length(dolt_commit('-am', 'ours: t, both refreshed')::text) AS committed;
SELECT (dolt_merge('mm_op', '-m', 'both refreshed both'))[3:4];
SELECT (SELECT c FROM mm.ob) AS ob, (SELECT c FROM mm.oa) AS oa,
	(SELECT sum(pk) FROM mm.t) AS t_sum;
SELECT count(*) AS clean FROM dolt_status;

-- Only the opaque views (and those reading them) are refreshed again:
-- three plain views and one opaque one, both sides refreshing all four,
-- refresh five times, not sixteen.
CREATE SEQUENCE mmc.s;
CREATE FUNCTION mmc.tick() RETURNS int LANGUAGE sql VOLATILE
	BEGIN ATOMIC SELECT (nextval('mmc.s') * 0)::int; END;
CREATE FUNCTION mmc.tsum() RETURNS int LANGUAGE sql STABLE
	AS 'SELECT coalesce(sum(pk), 0)::int FROM mm.t';
CREATE MATERIALIZED VIEW mm.k1 AS SELECT coalesce(sum(pk), 0) + mmc.tick() AS c FROM mm.t;
CREATE MATERIALIZED VIEW mm.k2 AS SELECT coalesce(sum(pk), 0) + mmc.tick() AS c FROM mm.t;
CREATE MATERIALIZED VIEW mm.k3 AS SELECT coalesce(sum(pk), 0) + mmc.tick() AS c FROM mm.t;
CREATE MATERIALIZED VIEW mm.k4 AS SELECT mmc.tsum() + mmc.tick() AS c;
SELECT length(dolt_commit('-Am', 'views counting their refreshes')::text) AS committed;
SELECT dolt_checkout('-b', 'mm_cnt');
INSERT INTO mm.t VALUES (100000);
REFRESH MATERIALIZED VIEW mm.k1;
REFRESH MATERIALIZED VIEW mm.k2;
REFRESH MATERIALIZED VIEW mm.k3;
REFRESH MATERIALIZED VIEW mm.k4;
SELECT length(dolt_commit('-am', 'theirs: t, all refreshed')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO mm.t VALUES (200000);
REFRESH MATERIALIZED VIEW mm.k1;
REFRESH MATERIALIZED VIEW mm.k2;
REFRESH MATERIALIZED VIEW mm.k3;
REFRESH MATERIALIZED VIEW mm.k4;
SELECT length(dolt_commit('-am', 'ours: t, all refreshed')::text) AS committed;
SELECT nextval('mmc.s') AS before \gset
SELECT (dolt_merge('mm_cnt', '-m', 'both refreshed all'))[3:4];
SELECT nextval('mmc.s') - :before - 1 AS refreshes;
SELECT (SELECT c FROM mm.k1) AS k1, (SELECT c FROM mm.k4) AS k4,
	(SELECT sum(pk) FROM mm.t) AS t_sum;
SELECT count(*) AS clean FROM dolt_status;

SELECT dolt_branch('-D', 'mm_other');
SELECT dolt_branch('-D', 'mm_fn');
SELECT dolt_branch('-D', 'mm_op');
SELECT dolt_branch('-D', 'mm_cnt');
DROP SCHEMA mm CASCADE;
DROP SCHEMA mmc CASCADE;
SELECT length(dolt_commit('-Am', 'mm gone')::text) AS committed;
