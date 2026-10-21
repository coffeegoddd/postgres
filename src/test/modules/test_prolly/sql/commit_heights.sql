-- Commits record their heights (1 + the most of their parents', a root's
-- 1), as Dolt's do, so what orders history by height reads only the
-- commits it needs: a commit that records one isn't walked beneath.  A
-- commit made before heights were recorded has its height found from its
-- history, each commit read once.
CREATE DATABASE regress_commit_heights;
\c regress_commit_heights
CREATE EXTENSION test_prolly;
CREATE FUNCTION counted(name text) RETURNS bigint LANGUAGE sql AS $$
	SELECT value FROM pg_stat_prolly WHERE pg_stat_prolly.name = counted.name
$$;
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE FUNCTION pg_temp.h(ref text) RETURNS bytea LANGUAGE sql
  AS $$ SELECT decode(dolt_hashof(ref), 'hex') $$;

CREATE SCHEMA ch VERSIONED;
CREATE TABLE ch.t (id int PRIMARY KEY, v text);
INSERT INTO ch.t VALUES (1, 'one');
SELECT length(dolt_commit('-Am', 'one')::text) AS committed;
SELECT dolt_branch('side');
UPDATE ch.t SET v = 'two' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'two')::text) AS committed;
SELECT dolt_checkout('side') IS NOT NULL AS on_side;
INSERT INTO ch.t VALUES (2, 'side');
SELECT length(dolt_commit('-Am', 'side 1')::text) AS committed;
INSERT INTO ch.t VALUES (3, 'side');
SELECT length(dolt_commit('-Am', 'side 2')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('side', '--no-ff', '-m', 'merged'))[3:4];

-- each commit's recorded height is its order in the log; a root's is 1,
-- and the merge's is 1 + its higher parent's (side's)
SELECT message, commit_order, test_commit_height(commit_hash) AS recorded
FROM dolt_log('main') ORDER BY commit_order, message;
SELECT test_commit_height(dolt_hashof('main')) =
	   test_commit_height(dolt_hashof('side')) + 1 AS merge_above_higher_parent;

-- the walk takes a recorded height at its word: one commit read
SELECT * FROM test_history_height(dolt_hashof('main'));

-- commits that record no height, as made before heights were: each one's
-- found from its parents', each commit read once (the 50 and the one
-- beneath them, which records its own)
SELECT test_commit_chain(dolt_hashof('main'), 50) AS old_head \gset
SELECT test_commit_height(:'old_head') AS recorded_on_old;
SELECT height = test_commit_height(dolt_hashof('main')) + 50 AS found_from_history,
	   reads
FROM test_history_height(:'old_head');

-- a commit made on such history records the height found: the walk beneath
-- it isn't made again
SELECT dolt_reset('--hard', :'old_head') IS NOT NULL AS reset;
INSERT INTO ch.t VALUES (4, 'on old');
SELECT length(dolt_commit('-Am', 'on old')::text) AS committed;
SELECT test_commit_height(dolt_hashof('main')) =
	   test_commit_height(dolt_hashof('main~51')) + 51 AS recorded_on_new;
SELECT * FROM test_history_height(dolt_hashof('main'));

-- dolt_serve_since reads the new commits and the frontier, not the
-- history beneath them: 200 commits, then what's new since the last but
-- one (main, read to resolve main~1, and main~1)
DO $$
BEGIN
	FOR i IN 1..200 LOOP
		PERFORM dolt_commit('--allow-empty', '-m', 'empty ' || i);
	END LOOP;
END
$$;
:reset
SELECT count(*) > 0 AS sent
FROM dolt_serve_since(ARRAY[pg_temp.h('main')], ARRAY[pg_temp.h('main~1')]);
:flush
SELECT counted('commits_read') AS commits_read;
-- and beneath history that records no heights, it reads that history, each
-- commit once for its height: the known commit and the 50 beneath it, and
-- the one beneath those, which records its own (52); and the wanted and
-- known commits read for the walk, and the wanted to make the known (3)
:reset
SELECT count(*) AS sent
FROM dolt_serve_since(ARRAY[decode(:'old_head', 'hex')],
					  ARRAY[decode(test_commit_chain(:'old_head', 1), 'hex')]);
:flush
SELECT counted('commits_read') AS commits_read;

\c postgres
DROP DATABASE regress_commit_heights;
