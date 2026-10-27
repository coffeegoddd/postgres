-- The history window: what each branch's lineage keeps here, as the
-- database's settings have it (ALTER DATABASE ... SET: its owner's or a
-- superuser's), never a session's, and what is done with history past it
-- (prolly.history_window_action): archived to prolly.history_archive and
-- found whole there before anything is severed, deleted, or kept.

\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/history_window'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset
SELECT 'liar://' || :'dir' || '/' || txid_current() || '_liar' AS liar_url \gset
SELECT current_database() AS home \gset
\set VERBOSITY terse
CREATE DATABASE regress_history_window;
\c regress_history_window

CREATE FUNCTION hw_err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g'),
						  '(file|liar)://[^ ]*', '<url>', 'g');
END $$;

CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
-- d(n): n days ago, at midnight
CREATE FUNCTION hw_day(n int) RETURNS timestamptz LANGUAGE sql AS $$
	SELECT date_trunc('day', now()) - make_interval(days => n)
$$;
CREATE FUNCTION hw_commit(msg text, ago int) RETURNS text LANGUAGE sql AS $$
	SELECT trim(both '{}' from dolt_commit('-Am', msg, '--date',
		(hw_day(ago) + interval '1 hour')::text)::text)
$$;
CREATE TABLE hw_names (hash text PRIMARY KEY, name text);
CREATE FUNCTION hw_name(h text) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce((SELECT name FROM hw_names WHERE hash = h), h)
$$;
INSERT INTO hw.t SELECT g, 'v' FROM generate_series(1, 200) g;
INSERT INTO hw_names VALUES (hw_commit('a10', 10), 'a10');
UPDATE hw.t SET v = 'a8' WHERE id < 100;
INSERT INTO hw_names VALUES (hw_commit('a8', 8), 'a8');
SELECT dolt_branch('long');
UPDATE hw.t SET v = 'a6' WHERE id < 50;
INSERT INTO hw_names VALUES (hw_commit('a6', 6), 'a6');
UPDATE hw.t SET v = 'a4' WHERE id < 25;
INSERT INTO hw_names VALUES (hw_commit('a4', 4), 'a4');
UPDATE hw.t SET v = 'a2' WHERE id < 10;
INSERT INTO hw_names VALUES (hw_commit('a2', 2), 'a2');
UPDATE hw.t SET v = 'a1' WHERE id < 5;
INSERT INTO hw_names VALUES (hw_commit('a1', 1), 'a1');
UPDATE hw.t SET v = 'a0' WHERE id < 3;
INSERT INTO hw_names VALUES (hw_commit('a0', 0), 'a0');
SELECT dolt_checkout('long');
INSERT INTO hw.t VALUES (500, 'long');
INSERT INTO hw_names VALUES (hw_commit('l7', 7), 'l7');
SELECT dolt_checkout('main');

-- No window: everything is kept, whatever the action.
SELECT * FROM prolly_history_policy(NULL);
SELECT count(*) AS planned FROM prolly_history_plan(NULL);

-- A window of three days, as the database has it; the long branch keeps
-- ten of its own; the first sever severs only what both are past.
ALTER DATABASE regress_history_window SET prolly.history_warm = '3 days';
SELECT dolt_history_window('long', 'warm', '10 days');
SELECT action, warm, min_commits FROM prolly_history_policy(NULL);
SELECT action, warm FROM prolly_history_policy('long');
SELECT hw_err($$SELECT dolt_history_window('long', 'archive', 'x')$$) AS not_a_branch_setting;
SELECT hw_err($$SELECT dolt_history_window(NULL, 'warm', '1 day')$$) AS null_branch,
	   hw_err($$SELECT dolt_history_window('long', NULL, '1 day')$$) AS null_setting;
SELECT kind, hw_name(commit_hash) AS commit, hw_name(parent_hash) AS parent, why
FROM prolly_history_plan(NULL) ORDER BY kind, commit, parent;

-- A session's settings don't count: not the action, not the window, not
-- the archive.  Archive mode with no archive severs nothing, and says so.
SET prolly.history_window_action = delete;
SET prolly.history_warm = '1 hour';
SET prolly.history_archive = 'file:///nowhere';
SELECT action, warm, archive FROM prolly_history_policy(NULL);
SELECT action, fragment, commits FROM dolt_history_sever();
SELECT count(*) AS fragments FROM prolly_history_horizon();
RESET prolly.history_window_action;
RESET prolly.history_warm;
RESET prolly.history_archive;
-- Nor does anyone but the database's owner sever, set a branch's window,
-- or make the database's settings.
CREATE ROLE regress_hw_other;
GRANT ALL ON SCHEMA public TO regress_hw_other;
SET ROLE regress_hw_other;
SET prolly.history_window_action = delete;
SELECT hw_err('SELECT * FROM dolt_history_sever()') AS other_sever;
SELECT hw_err($$SELECT dolt_history_window('main', 'warm', '1 hour')$$) AS other_window;
SELECT hw_err($$ALTER DATABASE regress_history_window SET prolly.history_window_action = delete$$) AS other_alter;
RESET ROLE;
SELECT count(*) AS fragments FROM prolly_history_horizon();

-- The archive is a URL: a remote's name, which others may move, isn't.
SELECT hw_err($$ALTER DATABASE regress_history_window SET prolly.history_archive = 'archive'$$) AS not_a_url;

-- An archive that says it has what it was sent, and keeps none of it, is
-- found out by reading it back: nothing is severed.
LOAD 'test_prolly';
ALTER DATABASE regress_history_window SET prolly.history_archive = :'liar_url';
SELECT hw_err('SELECT * FROM dolt_history_sever()') AS lying_archive;
SELECT count(*) AS fragments FROM prolly_history_horizon();

-- Archived: sent to the archive, read back whole from it, then severed.
-- A remote of the archive's name, moved by anyone, changes nothing.
ALTER DATABASE regress_history_window SET prolly.history_archive = :'url';
GRANT pg_prolly_write TO regress_hw_other;
SET ROLE regress_hw_other;
SELECT dolt_remote('add', 'archive', 'file:///elsewhere');
RESET ROLE;
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
-- (a sever rolled back leaves names in the archive; made again, it names
-- the same fragment the same)
BEGIN;
SELECT fragment, commits FROM dolt_history_sever();
ROLLBACK;
SELECT action, fragment, commits,
	   array(SELECT hw_name(b) FROM unnest(boundary) b ORDER BY 1) AS boundary,
	   sent AS sent_again
FROM dolt_history_sever();
SELECT fragment, state, commits, location = :'url' AS archived_there,
	   array(SELECT hw_name(t) FROM unnest(tops) t ORDER BY 1) AS tops
FROM prolly_history_horizon();
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT hw_name(commit_hash) AS commit FROM dolt_log('main') ORDER BY commit_order DESC;

-- A window by count: at least three commits, however old, for main; the
-- cut in whole days (cold_batch).  With long gone, its merge base pins
-- nothing.  A second fragment, archived beside the first: what the
-- archive has already isn't sent again.
SELECT dolt_branch('-D', 'long');
ALTER DATABASE regress_history_window SET prolly.history_warm = '1 hour';
ALTER DATABASE regress_history_window SET prolly.history_min_commits = 3;
ALTER DATABASE regress_history_window SET prolly.history_cold_batch = '1 day';
SELECT action, warm, min_commits, cold_batch FROM prolly_history_policy('main');
SELECT action, fragment, commits,
	   array(SELECT hw_name(b) FROM unnest(boundary) b ORDER BY 1) AS boundary,
	   sent < chunks AS sent_only_new
FROM dolt_history_sever();
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT hw_name(commit_hash) AS commit FROM dolt_log('main') ORDER BY commit_order DESC;

-- Repair from the archive, every fragment: the history reads again.
SELECT dolt_history_repair() AS repaired;
SELECT hw_name(commit_hash) AS commit FROM dolt_log('main') ORDER BY commit_order DESC;
SELECT v FROM dolt_at((SELECT hash FROM hw_names WHERE name = 'a8'), NULL::hw.t)
WHERE id = 1;

-- Another database archiving to the same archive names its own.
CREATE DATABASE regress_history_window_b;
\c regress_history_window_b
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
INSERT INTO hw.t VALUES (1, 'b');
SELECT length(dolt_commit('-Am', 'b1', '--date', '2020-01-01')::text) > 0 AS committed;
UPDATE hw.t SET v = 'b2';
SELECT length(dolt_commit('-am', 'b2', '--date', '2020-01-02')::text) > 0 AS committed;
UPDATE hw.t SET v = 'b3';
SELECT length(dolt_commit('-am', 'b3', '--date', '2020-01-03')::text) > 0 AS committed;
UPDATE hw.t SET v = 'b4';
SELECT length(dolt_commit('-am', 'b4', '--date', '2020-01-04')::text) > 0 AS committed;
ALTER DATABASE regress_history_window_b SET prolly.history_warm = '1 hour';
ALTER DATABASE regress_history_window_b SET prolly.history_archive = :'url';
SELECT action, fragment > 0 AS archived, commits FROM dolt_history_sever();
\c regress_history_window
DROP DATABASE regress_history_window_b;

-- Deleted, as the database says: nothing archived, the horizon notes it.
ALTER DATABASE regress_history_window SET prolly.history_window_action = delete;
SELECT action, fragment, commits,
	   array(SELECT hw_name(b) FROM unnest(boundary) b ORDER BY 1) AS boundary
FROM dolt_history_sever();
SELECT fragment, state, location FROM prolly_history_horizon() ORDER BY fragment DESC;
SELECT hw_name(commit_hash) AS commit FROM dolt_log('main') ORDER BY commit_order DESC;
ALTER DATABASE regress_history_window SET prolly.history_window_action = none;
SELECT action, fragment, commits FROM dolt_history_sever();

-- A branch's own window, with none for the database, cuts its lineage
-- (a branch's own commits are above its merge bases with the others, which
-- keep the paths to them: what a branch's window cuts is what every
-- lineage that reaches it lets go of).
CREATE DATABASE regress_history_window_c;
\c regress_history_window_c
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
INSERT INTO hw.t VALUES (1, 'c');
SELECT length(dolt_commit('-Am', 'c1', '--date', '2020-01-01')::text) > 0 AS committed;
UPDATE hw.t SET v = 'c2';
SELECT length(dolt_commit('-am', 'c2', '--date', '2020-01-02')::text) > 0 AS committed;
UPDATE hw.t SET v = 'c3';
SELECT length(dolt_commit('-am', 'c3', '--date', '2020-01-03')::text) > 0 AS committed;
UPDATE hw.t SET v = 'c4';
SELECT length(dolt_commit('-am', 'c4', '--date', '2020-01-04')::text) > 0 AS committed;
UPDATE hw.t SET v = 'c5';
SELECT length(dolt_commit('-am', 'c5', '--date', '2020-01-05')::text) > 0 AS committed;
SELECT count(*) AS planned_without FROM prolly_history_plan(NULL);
SELECT dolt_history_window('main', 'warm', '1 hour');
SELECT kind, count(*) FROM prolly_history_plan(NULL) GROUP BY kind ORDER BY kind;
\c regress_history_window
DROP DATABASE regress_history_window_c;

\c :home
DROP DATABASE regress_history_window;
DROP ROLE regress_hw_other;
