-- A checkout's work doesn't grow with the number of branches: its sweep
-- reads only the hidden branches (detached/, writer/), and finding a
-- branch by name looks up that name alone.  So a warm checkout, and
-- resolving a commit's hash, read as many refs nodes with 600 branches as
-- with 200 (as many as the refs tree's height, which is the same for
-- both).  And the sweep still finds the hidden branches among them.
-- (In a database of its own: other tests' branches would change the
-- refs tree's height.)

SELECT current_database() AS home \gset
CREATE DATABASE checkout_branch_count;
\c checkout_branch_count
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
CREATE FUNCTION pg_temp.work() RETURNS TABLE (counter text, grew int8)
LANGUAGE sql AS $$
	SELECT c, pg_temp.since(c)
	FROM unnest(ARRAY['refs_nodes_read', 'commits_read']) c
$$;
CREATE TEMP TABLE runs (branches int, what text, counter text, grew int8);
-- record(n, what): what the statement before read, with n branches
CREATE FUNCTION pg_temp.record(n int, what text) RETURNS void LANGUAGE sql AS $$
	INSERT INTO runs SELECT n, what, counter, grew FROM pg_temp.work()
$$;

CREATE SCHEMA cbc VERSIONED;
CREATE TABLE cbc.t (id int PRIMARY KEY);
INSERT INTO cbc.t VALUES (1);
SELECT (dolt_commit('-Am', 'cbc'))[1] AS head \gset
SELECT dolt_branch('cbc_a') IS NOT NULL AS made;
SELECT count(dolt_branch('cbc_many_' || g)) AS made
FROM generate_series(1, 199) g;
-- (cbc_a used once: what follows are switches)
SELECT (dolt_checkout('cbc_a'))[1];
SELECT (dolt_checkout('main'))[1];

-- A checkout to cbc_a and back, and resolving the commit by its hash.
:flush
SELECT count(*) >= 0 AS reset FROM pg_temp.work();
SELECT (dolt_checkout('cbc_a'))[1];
:flush
SELECT pg_temp.record(200, 'checkout');
SELECT (dolt_checkout('main'))[1];
:flush
SELECT pg_temp.record(200, 'checkout back');
SELECT dolt_hashof(:'head') = :'head' AS resolved;
:flush
SELECT pg_temp.record(200, 'resolve hash');

-- 400 more, and the same again.
SELECT count(dolt_branch('cbc_many_' || g)) AS made
FROM generate_series(200, 599) g;
SELECT (dolt_checkout('cbc_a'))[1];
SELECT (dolt_checkout('main'))[1];
SELECT dolt_hashof(:'head') = :'head' AS resolved;
:flush
SELECT count(*) >= 0 AS reset FROM pg_temp.work();
SELECT (dolt_checkout('cbc_a'))[1];
:flush
SELECT pg_temp.record(600, 'checkout');
SELECT (dolt_checkout('main'))[1];
:flush
SELECT pg_temp.record(600, 'checkout back');
SELECT dolt_hashof(:'head') = :'head' AS resolved;
:flush
SELECT pg_temp.record(600, 'resolve hash');

-- The same work with 400 more branches.
SELECT what, counter, a.grew = b.grew AS same, a.grew <= 16 AS bounded
FROM runs a JOIN runs b USING (what, counter)
WHERE a.branches = 200 AND b.branches = 600
ORDER BY what, counter;

-- Among the 600, the sweep still finds the hidden branches: a commit's
-- (none kept: let go of once left) and a writer's done with.
SET prolly.kept_checkouts = 0;
SELECT (dolt_checkout(:'head'))[1];
SELECT count(*) AS detached FROM pg_branch WHERE brname LIKE 'detached/%';
SELECT (dolt_checkout('main'))[1];
SELECT count(*) AS detached FROM pg_branch WHERE brname LIKE 'detached/%';
RESET prolly.kept_checkouts;
SELECT dolt_writer_mode('branch-auto');
SET dolt.writer = 'cbcw';
INSERT INTO cbc.t VALUES (2);
SELECT active_branch();
SELECT (dolt_integrate('-m', 'cbcw''s'))[2] AS fast_forward;
SELECT (dolt_checkout('main'))[1];
SELECT count(*) AS writer_branches FROM dolt_branches() WHERE kind = 'writer';
RESET dolt.writer;

\c :home
DROP DATABASE checkout_branch_count;
