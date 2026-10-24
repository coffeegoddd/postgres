-- History mounts: a table at every commit a commit reaches, as
-- dolt_history_table() gives it, as a foreign table
-- (prolly_mount(..., history => true)).  A scan streams a commit at a
-- time; an equality on commit_hash reads that commit alone, and the
-- primary key's bounds seek in each commit.
CREATE DATABASE regress_history_mounts;
\c regress_history_mounts
CREATE EXTENSION test_prolly;

-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked of (the session's counts flushed by the statement before).
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;

CREATE SCHEMA hs VERSIONED;
CREATE TABLE hs.users (tenant_id int, id int, email text, PRIMARY KEY (tenant_id, id));
INSERT INTO hs.users SELECT g / 100, g % 100, 'p' || g FROM generate_series(0, 999) g;
CREATE TABLE hs.bag (v int);
INSERT INTO hs.bag VALUES (1), (1), (2);
SELECT length(dolt_commit('-Am', 'first')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c1 \gset
UPDATE hs.users SET email = 'changed' WHERE tenant_id = 4 AND id = 42;
DELETE FROM hs.bag WHERE v = 2;
SELECT length(dolt_commit('-Am', 'second')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c2 \gset
-- a column added: older commits have it NULL
ALTER TABLE hs.users ADD COLUMN note text;
UPDATE hs.users SET note = 'n' WHERE id = 1;
-- a table made later: no rows before it
CREATE TABLE hs.late (id int PRIMARY KEY);
INSERT INTO hs.late VALUES (1);
SELECT length(dolt_commit('-Am', 'third')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c3 \gset
-- a merge: its branch's commits too, each once
SELECT dolt_branch('side');
SELECT dolt_checkout('side');
INSERT INTO hs.users VALUES (50, 1, 'side', NULL);
SELECT length(dolt_commit('-Am', 'on side')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS cside \gset
SELECT dolt_checkout('main');
INSERT INTO hs.users VALUES (60, 1, 'main', NULL);
SELECT length(dolt_commit('-Am', 'on main')::text) > 0 AS committed;
SELECT length(dolt_merge('side')::text) > 0 AS merged;
SELECT count(*) AS commits FROM dolt_log \gset

SELECT mount FROM prolly_mount('HEAD', 'hs', 'h', NULL, false, true);
SELECT a.attname, format_type(a.atttypid, a.atttypmod)
FROM pg_attribute a WHERE a.attrelid = 'h.users'::regclass AND a.attnum > 0
ORDER BY a.attnum;

-- the rows of every commit, as dolt_history_table() has them
SELECT count(DISTINCT commit_hash) = :commits AS every_commit FROM h.users;
SELECT (SELECT count(*) FROM (
		SELECT tenant_id, id, email, note, commit_hash, committer, commit_date FROM h.users
		EXCEPT ALL
		SELECT (row).tenant_id, (row).id, (row).email, (row).note, commit_hash, committer, commit_date
		FROM dolt_history_table(NULL::hs.users)) d) AS only_mount,
	(SELECT count(*) FROM (
		SELECT (row).tenant_id, (row).id, (row).email, (row).note, commit_hash, committer, commit_date
		FROM dolt_history_table(NULL::hs.users)
		EXCEPT ALL
		SELECT tenant_id, id, email, note, commit_hash, committer, commit_date FROM h.users) d) AS only_function;
-- and as its jsonb overload has them, by name
SELECT count(*) = (SELECT count(*) FROM h.users) AS as_many,
	count(*) FILTER (WHERE row->>'email' = 'changed') AS changed
FROM dolt_history_table('hs.users');
-- a row through its versions
SELECT email, note, commit_hash = :'c1' AS at_c1 FROM h.users
WHERE tenant_id = 4 AND id = 42 ORDER BY commit_date, email;
SELECT count(*) FILTER (WHERE note IS NOT NULL) AS noted,
	count(*) FILTER (WHERE commit_hash = :'c1') AS at_c1
FROM h.users WHERE id = 1;
-- keyless copies; a table only some commits have
SELECT v, count(*) FROM h.bag WHERE commit_hash = :'c1' GROUP BY v ORDER BY v;
SELECT v, count(*) FROM h.bag WHERE commit_hash = :'c2' GROUP BY v ORDER BY v;
SELECT count(*) = :commits - 2 AS later_commits FROM h.late;
-- the merged-in branch's commit is in the history once
SELECT count(*) FROM h.users WHERE commit_hash = :'cside' AND tenant_id = 50;

-- An equality on commit_hash reads that commit alone, a key equality
-- seeks in each commit; LIMIT stops the scan.
SET plan_cache_mode = force_generic_plan;
PREPARE at_commit(text) AS
	SELECT email FROM h.users WHERE commit_hash = $1 AND tenant_id = 4 AND id = 42;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
EXECUTE at_commit(:'c1');
EXECUTE at_commit(:'c1');
EXECUTE at_commit(:'c2');
DEALLOCATE at_commit;
RESET plan_cache_mode;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT email FROM h.users WHERE tenant_id = 4 AND id = 42;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT email FROM h.users LIMIT 3;
-- a commit the mount's commit doesn't reach, or no commit at all: none
SELECT count(*) FROM h.users WHERE commit_hash = 'nonsense';
SELECT count(*) FROM h.users WHERE commit_hash = NULL;
SELECT count(*) FROM h.users WHERE commit_hash = :'cside' AND tenant_id = 60;
-- joined to some commits: a nested loop reads each outer row's commit
-- alone; and to the log
SET enable_hashjoin = off;
SET enable_mergejoin = off;
EXPLAIN (COSTS OFF) SELECT u.email FROM (VALUES (:'c1'), (:'c2')) v(c)
JOIN h.users u ON u.commit_hash = v.c AND u.tenant_id = 4 AND u.id = 42;
SELECT u.email, u.commit_hash = :'c1' AS at_c1 FROM (VALUES (:'c1'), (:'c2')) v(c)
JOIN h.users u ON u.commit_hash = v.c AND u.tenant_id = 4 AND u.id = 42
ORDER BY 2 DESC;
RESET enable_hashjoin;
RESET enable_mergejoin;
SELECT c.message, u.email FROM dolt_log c
JOIN h.users u ON u.commit_hash = c.commit_hash AND u.tenant_id = 4 AND u.id = 42
ORDER BY c.commit_order;

-- Work: a selective lookup decodes the same, however long the history; a
-- full read decodes every commit's rows.
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') >= 0 AS counting;
SELECT email FROM h.users WHERE commit_hash = :'c2' AND tenant_id = 4 AND id = 42;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') AS one_commit_lookup;
SELECT length(dolt_commit('--allow-empty', '-m', 'more')::text) > 0,
	length(dolt_commit('--allow-empty', '-m', 'and more')::text) > 0;
SELECT prolly_refresh_mount('h') IS NOT NULL AS refreshed;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') >= 0 AS counting;
SELECT email FROM h.users WHERE commit_hash = :'c2' AND tenant_id = 4 AND id = 42;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') AS longer_history_lookup;
SELECT count(*) FROM h.users WHERE tenant_id = 4 AND id = 42;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') <= (:commits + 2) * 4 AS per_commit_seek;

-- A key changed in history: the commits keyed otherwise are read whole,
-- and still give their rows.
CREATE TABLE hs.rekeyed (a int, b int, PRIMARY KEY (a));
INSERT INTO hs.rekeyed VALUES (1, 10), (2, 20);
SELECT length(dolt_commit('-Am', 'keyed by a')::text) > 0 AS committed;
ALTER TABLE hs.rekeyed DROP CONSTRAINT rekeyed_pkey, ADD PRIMARY KEY (b);
SELECT length(dolt_commit('-Am', 'keyed by b')::text) > 0 AS committed;
SELECT mount FROM prolly_mount('HEAD', 'hs', 'hk', ARRAY['rekeyed'], false, true);
SELECT a, count(*) FROM hk.rekeyed WHERE b = 20 GROUP BY a;

-- Refused: a history mount with index trees, or of a table with a column
-- named as the commit's.
SELECT * FROM prolly_mount('HEAD', 'hs', 'hx', NULL, true, true);
CREATE TABLE hs.clash (id int PRIMARY KEY, commit_hash text);
SELECT length(dolt_commit('-Am', 'clash')::text) > 0 AS committed;
SELECT * FROM prolly_mount('HEAD', 'hs', 'hc', ARRAY['clash'], false, true);

SELECT prolly_unmount('h'), prolly_unmount('hk');
\c contrib_regression
DROP DATABASE regress_history_mounts;
