-- A fold looks each changed key up in the baseline's tree, then applies
-- the edits to that tree: the leaves the lookups read are kept for the
-- apply, so each is read from the chunk store once (nodes_read about the
-- nodes the fold writes; read twice, about twice them).  The same for a
-- transaction's own rows applied to the head under dolt.transaction_commit.

CREATE DATABASE fold_leaf_reads;
\c fold_leaf_reads
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE FUNCTION reads() RETURNS TABLE (rows_written bigint, read_per_written numeric)
LANGUAGE sql AS $$
	SELECT max(value) FILTER (WHERE name = 'fold_rows_written'),
		max(value) FILTER (WHERE name = 'nodes_read')::numeric /
			max(value) FILTER (WHERE name = 'nodes_written')
	FROM pg_stat_prolly
$$;
SET debug_prolly_verify_fold_caches = off;
CREATE SCHEMA s VERSIONED;
CREATE TABLE s.t (id int PRIMARY KEY, n int NOT NULL, pad text NOT NULL);
INSERT INTO s.t SELECT i, 0, repeat(md5(i::text), 5)
FROM generate_series(1, 20000) i;
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;

-- 300 rows spread over the table, each in a leaf of its own: a commit's
-- fold reads about one node for each it writes.
UPDATE s.t SET n = 1 WHERE id % 67 = 0;
:reset
SELECT length(dolt_commit('-am', 'spread')::text) > 0 AS committed;
:flush
SELECT rows_written, read_per_written < 1.5 AS leaves_read_once
FROM reads();

-- A transaction's own rows, applied to the head as it commits.
SET dolt.transaction_commit = on;
:reset
UPDATE s.t SET n = 2 WHERE id % 67 = 0;
RESET dolt.transaction_commit;
:flush
SELECT read_per_written < 1.5 AS leaves_read_once FROM reads();

\c contrib_regression
DROP DATABASE fold_leaf_reads;
