-- A hard reset discards the working set's changes in O(1), however many
-- rows they are and however much of them a fold took in: each table that
-- changed since the head is given the pages kept as of the head, and only
-- the rows the head and the commit differ by are written.  None of the
-- working set's rows is folded or written back for the discard, even when
-- a fold (the fold worker's, a checkpoint) took in part of them and left
-- the rest: the working set's distance from the commit isn't estimated.

SELECT current_database() AS home \gset
CREATE DATABASE dirty_discard;
\c dirty_discard

-- (the counters a move counts, read once the session's are flushed)
CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(n, coalesce((SELECT p.value FROM pg_stat_prolly p
										 WHERE p.name = n), 0))
	FROM unnest('{apply_rows,fold_rows_read,fold_rows_written,relations_restored,restores_lock_busy}'::text[]) n
$$;
CREATE FUNCTION since(before jsonb) RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(key, value::bigint - (before ->> key)::bigint)
	FROM jsonb_each_text(counted())
$$;
\set flush 'SELECT pg_stat_force_next_flush() IS NULL AS flushed;'
CREATE FUNCTION clean() RETURNS bool LANGUAGE sql AS
$$SELECT NOT EXISTS (SELECT FROM dolt_status)$$;

CREATE SCHEMA dd VERSIONED;
CREATE TABLE dd.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL);
CREATE INDEX t_v ON dd.t (v);
INSERT INTO dd.t SELECT g, 0, 'w' || g FROM generate_series(1, 20000) g;
CREATE FUNCTION t_digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM dd.t t
$$;

-- A history of commits each changing the same 50 rows, the target 20
-- commits back: its pages no longer kept, the head's kept.
SELECT (dolt_commit('-Am', 'base'))[1] AS target \gset
SELECT t_digest() AS at_target \gset
\pset tuples_only on
SELECT format('UPDATE dd.t SET v = %s WHERE id %% 400 = 0', r),
	   format('SELECT dolt_commit(''-am'', ''r%s'') IS NOT NULL', r)
FROM generate_series(1, 20) r \gexec
\pset tuples_only off

-- Half the table changed: two fifths of the changes folded, as the fold
-- worker leaves a working set it is behind on, and the rest after.  (The
-- unfolded rows are more than the folded rows' distance from the commit,
-- so the working set may be as near the commit as the head is: a reset
-- that estimated the working set's distance would fold and write it.)
UPDATE dd.t SET v = -1 WHERE id % 2 = 0 AND id <= 8000;
SELECT prolly_checkpoint() IS NOT NULL AS folded;
UPDATE dd.t SET v = -1 WHERE id % 2 = 0 AND id > 8000;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'target') IS NOT NULL AS reset;
:flush
SELECT since(:'before') AS moved \gset
-- the commit's own pages given back (its root kept), and none of the
-- working set's 10,000 folded or written
SELECT (:'moved'::jsonb ->> 'relations_restored')::int > 0 AS restored,
	   (:'moved'::jsonb ->> 'apply_rows')::int AS applied,
	   (:'moved'::jsonb ->> 'fold_rows_read')::int < 100 AS few_rows_folded,
	   (:'moved'::jsonb ->> 'restores_lock_busy')::int AS given_up;
SELECT t_digest() = :'at_target' AS at_target, clean();
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS by_index FROM dd.t WHERE v = -1;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- A reset to the head itself, every row of the working set changed and
-- nothing folded: the head's pages given back, no row written.
UPDATE dd.t SET v = -2;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', 'HEAD') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   (since(:'before') ->> 'fold_rows_read')::int AS folded,
	   t_digest() = :'at_target' AS at_head, clean();

-- The head made by a transaction of many commits (more than a relation
-- keeps snapshots: those the transaction's own commits kept and retired
-- are let go of as it goes, nobody else having seen them): kept all the
-- same, so a reset of a dirty working set to it gives its pages back.
CREATE FUNCTION history(n int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN
	FOR r IN 1..n LOOP
		UPDATE dd.t SET v = r WHERE id % 400 = 0;
		PERFORM dolt_commit('-am', 'h' || r);
	END LOOP;
	RETURN n;
END
$$;
SELECT history(20) AS commits;
SELECT t_digest() AS at_head \gset
UPDATE dd.t SET v = -3 WHERE id % 3 = 0;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', 'HEAD') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   t_digest() = :'at_head' AS at_head, clean();

-- Commits in a subtransaction rolled back: the earlier commit the
-- subtransaction's commits retired isn't let go of while the retiring may
-- yet roll back, so after the rollback it is still kept, the head a reset
-- gives back.
ALTER DATABASE dirty_discard SET prolly.history_min_commits = 1;
BEGIN;
UPDATE dd.t SET v = 31 WHERE id % 400 = 0;
SELECT dolt_commit('-am', 'c1') IS NOT NULL AS c1;
SAVEPOINT s;
UPDATE dd.t SET v = 32 WHERE id % 400 = 0;
SELECT dolt_commit('-am', 'c2') IS NOT NULL AS c2;
UPDATE dd.t SET v = 33 WHERE id % 400 = 0;
SELECT dolt_commit('-am', 'c3') IS NOT NULL AS c3;
ROLLBACK TO s;
COMMIT;
ALTER DATABASE dirty_discard RESET prolly.history_min_commits;
SELECT t_digest() AS at_head \gset
UPDATE dd.t SET v = -4 WHERE id % 3 = 0;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', 'HEAD') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   t_digest() = :'at_head' AS at_head, clean();

\c :home
DROP DATABASE dirty_discard;
