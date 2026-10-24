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

-- Columns by identity: one renamed since is read by its name then, one
-- retyped since converted (a value that doesn't convert NULL, with a
-- warning), as dolt_history_table() reads them.
CREATE TABLE hs.ident (id int PRIMARY KEY, old_name text, n text);
INSERT INTO hs.ident VALUES (1, 'one', '10'), (2, 'two', 'x');
SELECT length(dolt_commit('-Am', 'ident')::text) > 0 AS committed;
ALTER TABLE hs.ident RENAME COLUMN old_name TO new_name;
UPDATE hs.ident SET n = '20' WHERE id = 2;
ALTER TABLE hs.ident ALTER COLUMN n TYPE int USING n::int;
SELECT length(dolt_commit('-Am', 'renamed and retyped')::text) > 0 AS committed;
SELECT mount FROM prolly_mount('HEAD', 'hs', 'hi', ARRAY['ident'], false, true);
\set VERBOSITY terse
SELECT id, new_name, n FROM hi.ident ORDER BY commit_date, id;
SELECT (SELECT count(*) FROM (
		SELECT id, new_name, n, commit_hash FROM hi.ident
		EXCEPT ALL
		SELECT (row).id, (row).new_name, (row).n, commit_hash
		FROM dolt_history_table(NULL::hs.ident)) d) AS only_mount,
	(SELECT count(*) FROM (
		SELECT (row).id, (row).new_name, (row).n, commit_hash
		FROM dolt_history_table(NULL::hs.ident)
		EXCEPT ALL
		SELECT id, new_name, n, commit_hash FROM hi.ident) d) AS only_function;
\set VERBOSITY default
SELECT prolly_unmount('hi');

-- Who may read each version: a table dropped and made again by its name
-- is another table, so a role granted the new one is refused the old
-- one's commits, as dolt_history_table() by name refuses them.
CREATE TABLE hs.again (id int PRIMARY KEY, v text);
INSERT INTO hs.again VALUES (1, 'old1'), (2, 'old2');
SELECT length(dolt_commit('-Am', 'first again')::text) > 0 AS committed;
DROP TABLE hs.again;
SELECT length(dolt_commit('-Am', 'dropped again')::text) > 0 AS committed;
CREATE TABLE hs.again (id int PRIMARY KEY, v text);
INSERT INTO hs.again VALUES (3, 'new');
SELECT length(dolt_commit('-Am', 'made again')::text) > 0 AS committed;
SELECT mount FROM prolly_mount('HEAD', 'hs', 'ha', ARRAY['again'], false, true);
SELECT v FROM ha.again ORDER BY v;
CREATE ROLE regress_history_reader;
GRANT USAGE ON SCHEMA hs, ha TO regress_history_reader;
GRANT SELECT ON hs.again, ha.again TO regress_history_reader;
SET ROLE regress_history_reader;
\set VERBOSITY terse
SELECT v FROM ha.again ORDER BY v;
SELECT count(*) FROM dolt_history_table('hs.again');
\set VERBOSITY default
RESET ROLE;
REVOKE ALL ON hs.again, ha.again FROM regress_history_reader;
REVOKE ALL ON SCHEMA hs, ha FROM regress_history_reader;
DROP ROLE regress_history_reader;
SELECT prolly_unmount('ha');

-- A commit whose table can't be read here: an error, as a mount's.
CREATE EXTENSION test_prolly_ordering VERSION '1.0';
CREATE TABLE hs.ordered (k test_ordering PRIMARY KEY, v int);
INSERT INTO hs.ordered VALUES (ROW(1), 1);
SELECT length(dolt_commit('-Am', 'ordered by an extension')::text) > 0 AS committed;
ALTER TABLE hs.ordered DROP CONSTRAINT ordered_pkey, ADD COLUMN id int;
UPDATE hs.ordered SET id = 1;
ALTER TABLE hs.ordered ADD PRIMARY KEY (id);
SELECT length(dolt_commit('-Am', 'keyed by id')::text) > 0 AS committed;
SELECT mount FROM prolly_mount('HEAD', 'hs', 'ho', ARRAY['ordered'], false, true);
SELECT dolt_hashof('HEAD') AS ordered_head \gset
SELECT count(*) FROM ho.ordered WHERE commit_hash = :'ordered_head';
\set VERBOSITY terse
SELECT count(*) FROM ho.ordered;
\set VERBOSITY default
SELECT prolly_unmount('ho');
DROP TABLE hs.ordered;
DROP EXTENSION test_prolly_ordering;
SELECT length(dolt_commit('-Am', 'no ordered')::text) > 0 AS committed;

-- Work: planning reads no history; LIMIT reads few commits; a join to
-- every commit reads each once (reachability found once per scan).
SELECT count(*) FILTER (WHERE length(dolt_commit('--allow-empty', '-m', 'empty ' || g)::text) > 0) AS empties
FROM generate_series(1, 30) g \gset
SELECT prolly_refresh_mount('h') IS NOT NULL AS refreshed;
SELECT count(*) AS commits FROM dolt_log \gset
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('commits_read') >= 0 AS counting;
EXPLAIN (COSTS OFF) SELECT email FROM h.users WHERE tenant_id = 4 AND id = 42;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('commits_read') <= 2 AS planning_reads_no_history;
SELECT count(*) FROM (SELECT email FROM h.users LIMIT 3) x;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('commits_read') < :commits / 4 AS limit_reads_few;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT count(*) FROM dolt_log c
JOIN LATERAL (SELECT email FROM h.users u WHERE u.commit_hash = c.commit_hash
			  AND u.tenant_id = 4 AND u.id = 42 OFFSET 0) x ON true;
RESET enable_hashjoin;
RESET enable_mergejoin;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('commits_read') < :commits * 5 AS join_reads_each_commit_once;
SELECT mount, history FROM prolly_mounts() ORDER BY mount;

-- Refused: a history mount with index trees, or of a table with a column
-- named as the commit's.
SELECT * FROM prolly_mount('HEAD', 'hs', 'hx', NULL, true, true);
CREATE TABLE hs.clash (id int PRIMARY KEY, commit_hash text);
SELECT length(dolt_commit('-Am', 'clash')::text) > 0 AS committed;
SELECT * FROM prolly_mount('HEAD', 'hs', 'hc', ARRAY['clash'], false, true);

SELECT prolly_unmount('h'), prolly_unmount('hk');
\c contrib_regression
DROP DATABASE regress_history_mounts;
