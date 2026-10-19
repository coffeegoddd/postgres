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

SELECT dolt_branch('-D', 'mm_other');
DROP SCHEMA mm CASCADE;
SELECT length(dolt_commit('-Am', 'mm gone')::text) AS committed;
