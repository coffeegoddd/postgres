-- The history window: what each branch's lineage keeps here, as the
-- database's settings have it (ALTER DATABASE ... SET: its owner's or a
-- superuser's), never a session's, and what is done with history past it
-- (prolly.history_window_action): archived to prolly.history_archive and
-- found whole there before anything is severed, deleted, or kept.

\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/history_window'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset
SELECT 'liar://' || :'dir' || '/' || txid_current() || '_liar' AS liar_url \gset
SELECT 'slow://' || :'dir' || '/' || txid_current() || '_slow' AS slow_url \gset
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

-- Every branch's history has a window: a month, unless the database
-- sets another.  It can't be unset, empty or nothing long, nor a branch's.
SELECT * FROM prolly_history_policy(NULL);
SELECT count(*) AS planned FROM prolly_history_plan(NULL);
SELECT hw_err($$SET prolly.history_window = ''$$) AS empty_refused,
	   hw_err($$SET prolly.history_window = '0'$$) AS zero_refused;
SELECT hw_err($$ALTER DATABASE regress_history_window SET prolly.history_window = ''$$) AS database_empty_refused;

-- A window of three days, as the database has it; the long branch keeps
-- ten of its own; the first sever severs only what both are past.
ALTER DATABASE regress_history_window SET prolly.history_window = '3 days';
SELECT dolt_history_window('long', 'window', '10 days');
SELECT action, time_window, min_commits FROM prolly_history_policy(NULL);
SELECT action, time_window FROM prolly_history_policy('long');
SELECT hw_err($$SELECT dolt_history_window('long', 'archive', 'x')$$) AS not_a_branch_setting;
SELECT hw_err($$SELECT dolt_history_window('long', 'window', '0 seconds')$$) AS branch_none_refused;
SELECT hw_err($$SELECT dolt_history_window(NULL, 'window', '1 day')$$) AS null_branch,
	   hw_err($$SELECT dolt_history_window('long', NULL, '1 day')$$) AS null_setting;
SELECT kind, hw_name(commit_hash) AS commit, hw_name(parent_hash) AS parent, why
FROM prolly_history_plan(NULL) ORDER BY kind, commit, parent;

-- A session's settings don't count: not the action, not the window, not
-- the archive.  Archive mode with no archive severs nothing, and says so.
SET prolly.history_window_action = delete;
SET prolly.history_window = '1 hour';
SET prolly.history_archive = 'file:///nowhere';
SELECT action, time_window, archive FROM prolly_history_policy(NULL);
SELECT action, fragment, commits FROM dolt_history_sever();
SELECT count(*) AS fragments FROM prolly_history_horizon();
RESET prolly.history_window_action;
RESET prolly.history_window;
RESET prolly.history_archive;
-- Nor does anyone but the database's owner sever, set a branch's window,
-- or make the database's settings.
CREATE ROLE regress_hw_other;
GRANT ALL ON SCHEMA public TO regress_hw_other;
SET ROLE regress_hw_other;
SET prolly.history_window_action = delete;
SELECT hw_err('SELECT * FROM dolt_history_sever()') AS other_sever;
SELECT hw_err($$SELECT dolt_history_window('main', 'window', '1 hour')$$) AS other_window;
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

-- An archive that doesn't answer: the sever gives up at the database's
-- time limit, nothing severed.
ALTER DATABASE regress_history_window SET prolly.history_archive = 'stall:///nowhere';
ALTER DATABASE regress_history_window SET prolly.history_archive_timeout = '1 second';
\c regress_history_window
LOAD 'test_prolly';
SELECT hw_err('SELECT * FROM dolt_history_sever()') AS stalled;
SELECT count(*) AS fragments FROM prolly_history_horizon();
-- An archive that is slow but goes on: the limit is one without progress,
-- so it finishes, past the limit in all (four tenths of a second a chunk).
-- Rolled back, so the archive below is the first.
ALTER DATABASE regress_history_window SET prolly.history_archive = :'slow_url';
\c regress_history_window
LOAD 'test_prolly';
BEGIN;
SELECT sent * 0.4 > 1 AS longer_than_the_limit, fragment > 0 AS severed
FROM dolt_history_sever();
ROLLBACK;
ALTER DATABASE regress_history_window RESET prolly.history_archive_timeout;
\c regress_history_window
LOAD 'test_prolly';

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
-- cut in whole days (prolly.history_batch).  With long gone, its merge base pins
-- nothing.  A second fragment, archived elsewhere than the first: what
-- the fragment reaches is sent there whole, made before it too, and the
-- archive checked for all of it, since a repair of it from there receives
-- that.  (What is sent is what the archive lacks of all the fragment
-- reaches; what the fragment made, its chunks, may be fewer.)  The first
-- fragment's data, freed since, isn't: it is archived at the first.
SELECT dolt_branch('-D', 'long');
ALTER DATABASE regress_history_window SET prolly.history_window = '1 hour';
ALTER DATABASE regress_history_window SET prolly.history_min_commits = 3;
ALTER DATABASE regress_history_window SET prolly.history_batch = '1 day';
SELECT action, time_window, min_commits, batch FROM prolly_history_policy('main');
SELECT :'url' || '_second' AS url_second \gset
ALTER DATABASE regress_history_window SET prolly.history_archive = :'url_second';
SELECT action, fragment, commits,
	   array(SELECT hw_name(b) FROM unnest(boundary) b ORDER BY 1) AS boundary,
	   sent > chunks AS sent_what_it_reaches
FROM dolt_history_sever();
SELECT fragment, location = :'url_second' AS archived_second
FROM prolly_history_horizon() ORDER BY fragment;
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT hw_name(commit_hash) AS commit FROM dolt_log('main') ORDER BY commit_order DESC;

-- Repair from the second archive: it holds the second fragment, never
-- the first's data, so only the second is repaired from it.
SELECT dolt_history_repair(:'url_second', 1) AS repaired_second;
SELECT hw_err($$SELECT dolt_history_repair('$$ || :'url_second' || $$')$$) AS first_from_second;
SELECT fragment, location = :'url' AS archived_first
FROM prolly_history_horizon() ORDER BY fragment;

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
ALTER DATABASE regress_history_window_b SET prolly.history_window = '1 hour';
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

-- A branch's own window, narrower than the database's (a long one here),
-- cuts its lineage (a branch's own commits are above its merge bases with
-- the others, which keep the paths to them: what a branch's window cuts is
-- what every lineage that reaches it lets go of).
CREATE DATABASE regress_history_window_c;
ALTER DATABASE regress_history_window_c SET prolly.history_window = '1000 years';
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
SELECT dolt_history_window('main', 'window', '1 hour');
SELECT kind, count(*) FROM prolly_history_plan(NULL) GROUP BY kind ORDER BY kind;
\c regress_history_window
DROP DATABASE regress_history_window_c;

-- A branch's own window follows it: renamed with it, let go of with it,
-- so a branch made again by the name has the database's.
SELECT dolt_branch('own') IS NOT NULL AS made;
SELECT dolt_history_window('own', 'window', '5 days');
SELECT dolt_branch('-m', 'own', 'owned') IS NOT NULL AS renamed;
SELECT time_window FROM prolly_history_policy('owned');
SELECT dolt_branch('-D', 'owned') IS NOT NULL AS dropped;
SELECT dolt_branch('owned') IS NOT NULL AS made_again;
SELECT time_window FROM prolly_history_policy('owned');
SELECT dolt_branch('-D', 'owned') IS NOT NULL AS dropped;

-- A branch's window narrower than the database's cuts the branch's own
-- history: what else names its commits (its page roots, its log, a
-- tracking branch) keeps only those, not the database's window.
CREATE DATABASE regress_history_window_d;
\c regress_history_window_d
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
INSERT INTO hw.t VALUES (1, 'a10');
SELECT length(dolt_commit('-Am', 'a10', '--date', (now() - interval '10 days')::text)::text) > 0 AS committed;
UPDATE hw.t SET v = 'a8';
SELECT length(dolt_commit('-am', 'a8', '--date', (now() - interval '8 days')::text)::text) > 0 AS committed;
UPDATE hw.t SET v = 'a1';
SELECT length(dolt_commit('-am', 'a1', '--date', (now() - interval '1 day')::text)::text) > 0 AS committed;
UPDATE hw.t SET v = 'a0';
SELECT length(dolt_commit('-am', 'a0', '--date', (now() - interval '2 hours')::text)::text) > 0 AS committed;
ALTER DATABASE regress_history_window_d SET prolly.history_window = '9 days';
SELECT dolt_history_window('main', 'window', '3 days');
SELECT p.kind, l.message
FROM prolly_history_plan(NULL) p LEFT JOIN dolt_log('main') l ON l.commit_hash = p.commit_hash
ORDER BY 1, 2;
-- The window as a sever has it, for others to ask (the page roots' kept
-- inside it): main's three days hold a1 and a0, not a8, which the
-- database's nine days would.
SELECT l.message, prolly_history_in_window('main', l.commit_hash) AS main,
	prolly_history_in_window(NULL, l.commit_hash) AS any
FROM dolt_log('main') l ORDER BY l.date;
-- (a count keeps its newest, whatever their age)
SELECT dolt_history_window('main', 'min_commits', '3');
SELECT l.message FROM dolt_log('main') l
WHERE prolly_history_in_window('main', l.commit_hash) ORDER BY l.date;
\c regress_history_window
DROP DATABASE regress_history_window_d;

-- The graph kept: a sever keeps every commit object and severs only the
-- data, so ancestry goes across the horizon.  A tag on main's history
-- keeps only its own commit's data, not the path to it; a merge or a
-- three-dot diff of it finds it as the base.  A branch forked before the
-- horizon keeps only its merge base's data, and a merge of it across the
-- severed path finds that base (never no base, the histories unrelated).
CREATE DATABASE regress_history_window_e;
\c regress_history_window_e
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
CREATE FUNCTION hw_err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g');
END $$;
INSERT INTO hw.t VALUES (1, 'a10'), (2, 'a10');
SELECT length(dolt_commit('-Am', 'a10', '--date', (now() - interval '10 days')::text)::text) > 0 AS committed;
SELECT dolt_tag('old', 'HEAD') IS NOT NULL AS tagged;
UPDATE hw.t SET v = 'a8' WHERE id = 1;
SELECT length(dolt_commit('-am', 'a8', '--date', (now() - interval '8 days')::text)::text) > 0 AS committed;
SELECT dolt_branch('side') IS NOT NULL AS branched;
UPDATE hw.t SET v = 'a6' WHERE id = 1;
SELECT length(dolt_commit('-am', 'a6', '--date', (now() - interval '6 days')::text)::text) > 0 AS committed;
UPDATE hw.t SET v = 'a4' WHERE id = 1;
SELECT length(dolt_commit('-am', 'a4', '--date', (now() - interval '4 days')::text)::text) > 0 AS committed;
UPDATE hw.t SET v = 'a0' WHERE id = 1;
SELECT length(dolt_commit('-am', 'a0', '--date', (now() - interval '2 hours')::text)::text) > 0 AS committed;
SELECT dolt_checkout('side') IS NOT NULL AS checked_out;
UPDATE hw.t SET v = 's1' WHERE id = 2;
SELECT length(dolt_commit('-am', 's1', '--date', (now() - interval '1 hour')::text)::text) > 0 AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS checked_out;
ALTER DATABASE regress_history_window_e SET prolly.history_window_action = delete;
\c regress_history_window_e
SELECT p.kind, l.message, p.why
FROM prolly_history_plan(now() - interval '3 days') p
	LEFT JOIN dolt_log('main') l ON l.commit_hash = p.commit_hash
WHERE p.kind IN ('severed', 'pinned') ORDER BY 1, 2;
SELECT fragment, commits FROM dolt_history_drop(now() - interval '3 days');
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
-- (every commit listed, the severed ones' data gone)
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC;
SELECT hw_err(format('SELECT count(*) FROM dolt_at(%L, NULL::hw.t)',
	(SELECT commit_hash FROM dolt_log('main') WHERE message = 'a6'))) AS a6_rows;
SELECT table_name, rows_modified FROM dolt_diff_stat('old...main');
SELECT table_name, rows_modified FROM dolt_diff_stat('side...main');
SELECT (dolt_merge('old'))[2:4] AS merge_of_tag;
SELECT (dolt_merge('side'))[2:4] AS merge_of_side;
SELECT id, v FROM hw.t ORDER BY id;
\c regress_history_window
DROP DATABASE regress_history_window_e;

-- A commit a reader's ref names keeps its own data past a sever and a
-- full collection: a remote-tracking branch's (origin/main at an 8-day-old
-- commit) and a snapshot mount's pin.  Only the history behind them is
-- severed.
SELECT :'url' || '_o' AS url_o \gset
CREATE DATABASE regress_history_window_o;
\c regress_history_window_o
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
INSERT INTO hw.t VALUES (1, 'a10');
SELECT length(dolt_commit('-Am', 'a10', '--date', (now() - interval '10 days')::text)::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS a10 \gset
SELECT mount FROM prolly_mount(:'a10', 'hw', 'old');
UPDATE hw.t SET v = 'a9';
SELECT length(dolt_commit('-am', 'a9', '--date', (now() - interval '9 days')::text)::text) > 0 AS committed;
UPDATE hw.t SET v = 'a8';
SELECT length(dolt_commit('-am', 'a8', '--date', (now() - interval '8 days')::text)::text) > 0 AS committed;
SELECT dolt_remote('add', 'origin', :'url_o') IS NOT NULL AS added;
SELECT dolt_push('origin', 'main') IS NOT NULL AS pushed;
UPDATE hw.t SET v = 'a7';
SELECT length(dolt_commit('-am', 'a7', '--date', (now() - interval '7 days')::text)::text) > 0 AS committed;
UPDATE hw.t SET v = 'a0';
SELECT length(dolt_commit('-am', 'a0', '--date', (now() - interval '2 hours')::text)::text) > 0 AS committed;
ALTER DATABASE regress_history_window_o SET prolly.history_window = '3 days';
ALTER DATABASE regress_history_window_o SET prolly.history_window_action = delete;
\c regress_history_window_o
SELECT p.kind, l.message, p.why FROM prolly_history_plan(NULL) p
	LEFT JOIN dolt_log('main') l ON l.commit_hash = p.commit_hash
WHERE p.kind IN ('severed', 'pinned') ORDER BY 1, 2;
SELECT fragment, commits FROM dolt_history_sever();
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT v FROM dolt_at('remotes/origin/main', NULL::hw.t);
SELECT table_name, rows_modified FROM dolt_diff_stat('remotes/origin/main', 'main');
SELECT v FROM old.t;
\c regress_history_window
DROP DATABASE regress_history_window_o;

-- A merge in progress on another branch, begun by another session, keeps
-- its other side's data and its base's past a sever and a full
-- collection made from this one: the merge resolves with the right rows.
CREATE DATABASE regress_history_window_m;
\c regress_history_window_m
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
INSERT INTO hw.t VALUES (1, 'a'), (2, 'a');
SELECT length(dolt_commit('-Am', 'a10', '--date', (now() - interval '10 days')::text)::text) > 0 AS committed;
SELECT dolt_branch('feat') IS NOT NULL AS branched;
UPDATE hw.t SET v = 'm' WHERE id = 1;
SELECT length(dolt_commit('-am', 'a8', '--date', (now() - interval '8 days')::text)::text) > 0 AS committed;
UPDATE hw.t SET v = 'm0' WHERE id = 1;
SELECT length(dolt_commit('-am', 'a0', '--date', (now() - interval '2 hours')::text)::text) > 0 AS committed;
SELECT dolt_checkout('feat') IS NOT NULL AS checked_out;
UPDATE hw.t SET v = 'f' WHERE id IN (1, 2);
SELECT length(dolt_commit('-am', 'f9', '--date', (now() - interval '9 days')::text)::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS f9 \gset
INSERT INTO hw.t VALUES (3, 'f1');
SELECT length(dolt_commit('-am', 'f1', '--date', (now() - interval '1 hour')::text)::text) > 0 AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS checked_out;
SELECT dolt_branch('work') IS NOT NULL AS branched;
SELECT dolt_checkout('work') IS NOT NULL AS checked_out;
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge(:'f9'))[3] AS conflicts;
-- (this session leaves the merge in progress; another severs)
\c regress_history_window_m
ALTER DATABASE regress_history_window_m SET prolly.history_window = '3 days';
ALTER DATABASE regress_history_window_m SET prolly.history_window_action = delete;
\c regress_history_window_m
SELECT p.kind, p.why, p.commit_hash = :'f9' AS is_f9 FROM prolly_history_plan(NULL) p
WHERE p.kind = 'pinned' ORDER BY 1, 2, 3;
SELECT fragment > 0 AS severed FROM dolt_history_sever();
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
\c regress_history_window_m
SELECT dolt_checkout('work') IS NOT NULL AS checked_out;
SET dolt.allow_commit_conflicts = on;
SELECT (their_row).id, (their_row).v, (base_row).v AS base_v
FROM dolt_conflicts(NULL::hw.t) ORDER BY 1;
SELECT dolt_conflicts_resolve('--theirs', 'hw.t');
SELECT length(dolt_commit('-am', 'merged')::text) > 0 AS committed;
SELECT id, v FROM hw.t ORDER BY id;
\c regress_history_window
DROP DATABASE regress_history_window_m;

-- What a reader and a collection read of the horizon doesn't grow with
-- the commits ever severed: a commit's severing is a point lookup, and a
-- full collection reads only the severed commits whose data may be here
-- still (those a later sever hasn't found freed).
CREATE DATABASE regress_history_window_n;
\c regress_history_window_n
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
INSERT INTO hw.t VALUES (1, 'start');
SELECT length(dolt_commit('-Am', 'start', '--date', '2020-01-01')::text) > 0 AS committed;
DO $$
BEGIN
	FOR i IN 1 .. 120 LOOP
		UPDATE hw.t SET v = 'c' || i;
		PERFORM dolt_commit('-am', 'c' || i, '--date',
							(timestamptz '2020-01-01' + make_interval(hours => i))::text);
	END LOOP;
END $$;
UPDATE hw.t SET v = 'mid';
SELECT length(dolt_commit('-am', 'mid', '--date', '2020-01-25')::text) > 0 AS committed;
UPDATE hw.t SET v = 'later';
SELECT length(dolt_commit('-am', 'later', '--date', '2020-02-01')::text) > 0 AS committed;
UPDATE hw.t SET v = 'now';
SELECT length(dolt_commit('-am', 'now')::text) > 0 AS committed;
ALTER DATABASE regress_history_window_n SET prolly.history_window_action = delete;
\c regress_history_window_n
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
SELECT fragment, commits FROM dolt_history_drop('2020-01-20');
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
-- (a second sever, of one commit, finds the first's data freed: their
-- trees each noted, should one be written again, which the collection
-- after reads once)
SELECT fragment, commits FROM dolt_history_drop('2020-01-26');
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('horizon_entries_read') >= 0 AS counting;
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('horizon_entries_read') BETWEEN 121 AND 121 + 10 AS reads_trees_noted;
-- (a third lets go of those still freed: a collection reads few again)
SELECT fragment, commits FROM dolt_history_drop('2020-02-02');
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('horizon_entries_read') >= 0 AS counting;
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('horizon_entries_read') < 10 AS collection_reads_few;
SELECT kind, count(*) FROM prolly_history_plan(NULL) GROUP BY 1 ORDER BY 1;
SELECT count(*) FROM dolt_at('HEAD', NULL::hw.t);
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('horizon_entries_read') < 10 AS plan_and_read_few;
\c regress_history_window
DROP DATABASE regress_history_window_n;

-- A severed commit's pending record let go of, its root freed, and the
-- same tree written again later (a commit of the same rows), then let go
-- of by every live commit: a full collection frees it, not following it
-- from the severed commit as a pending record wouldn't either.
CREATE DATABASE regress_history_window_q;
ALTER DATABASE regress_history_window_q SET prolly.root_interval = '1h';
\c regress_history_window_q
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
INSERT INTO hw.t VALUES (1, 'start');
SELECT length(dolt_commit('-Am', 'start', '--date', '2020-01-01')::text) > 0 AS committed;
SELECT prolly_root_hash(dolt_hashof('HEAD')) AS start_root \gset
UPDATE hw.t SET v = 'b';
SELECT length(dolt_commit('-am', 'b', '--date', '2020-01-02')::text) > 0 AS committed;
UPDATE hw.t SET v = 'c';
SELECT length(dolt_commit('-am', 'c', '--date', '2020-01-03')::text) > 0 AS committed;
ALTER DATABASE regress_history_window_q SET prolly.history_window_action = delete;
\c regress_history_window_q
SELECT fragment, commits FROM dolt_history_drop('2020-01-02');
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT test_chunk_has(:'start_root') AS start_root_kept;
-- (the next sever finds start's root freed: its pending record let go of)
SELECT fragment, commits FROM dolt_history_drop('2020-01-03');
UPDATE hw.t SET v = 'start';
SELECT length(dolt_commit('-am', 'again', '--date', '2020-01-04')::text) > 0 AS committed;
SELECT prolly_root_hash(dolt_hashof('HEAD')) = :'start_root' AS same_root;
UPDATE hw.t SET v = 'now';
SELECT length(dolt_commit('-am', 'now')::text) > 0 AS committed;
SELECT fragment, commits FROM dolt_history_drop('2020-01-05');
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT test_chunk_has(:'start_root') AS start_root_kept;
SELECT count(*) FROM dolt_at('HEAD', NULL::hw.t);
\c regress_history_window
DROP DATABASE regress_history_window_q;

-- Delete mode with many tags: each keeps only its own commit's data, so
-- what is outside the window and untagged is severed.
CREATE DATABASE regress_history_window_t;
\c regress_history_window_t
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
INSERT INTO hw.t VALUES (1, 'start');
SELECT length(dolt_commit('-Am', 'start', '--date', '2020-01-01')::text) > 0 AS committed;
DO $$
BEGIN
	FOR i IN 1 .. 400 LOOP
		UPDATE hw.t SET v = 'c' || i;
		PERFORM dolt_commit('-am', 'c' || i, '--date',
							(timestamptz '2020-01-01' + make_interval(hours => i))::text);
		IF i % 2 = 0 THEN
			PERFORM dolt_tag('t' || i, 'HEAD');
		END IF;
	END LOOP;
END $$;
UPDATE hw.t SET v = 'now';
SELECT length(dolt_commit('-am', 'now')::text) > 0 AS committed;
ALTER DATABASE regress_history_window_t SET prolly.history_window = '1 day';
ALTER DATABASE regress_history_window_t SET prolly.history_window_action = delete;
\c regress_history_window_t
SELECT kind, why, count(*) FROM prolly_history_plan(NULL) GROUP BY 1, 2 ORDER BY 1, 2;
SELECT fragment, commits FROM dolt_history_sever();
SELECT count(*) AS logged FROM dolt_log('main');
\c regress_history_window
DROP DATABASE regress_history_window_t;

-- Ages by a clock that only goes forward along history: a commit dated
-- long ago (--date, a skewed clock) on top of recent ones severs none of
-- them; one dated in the future keeps itself and what is above it until
-- its date; history merged in with old dates of its own ages by them.
CREATE DATABASE regress_history_window_f;
-- (its commits' hashes vary with now(): a warning naming one kept out)
ALTER DATABASE regress_history_window_f SET client_min_messages = error;
\c regress_history_window_f
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
CREATE FUNCTION hw_c(msg text, at timestamptz) RETURNS bool LANGUAGE sql AS $$
	UPDATE hw.t SET v = msg WHERE id = 1;
	SELECT length(dolt_commit('-Am', msg, '--date', at::text)::text) > 0
$$;
INSERT INTO hw.t VALUES (1, 'start');
SELECT hw_c('a10', now() - interval '10 days') AND hw_c('a8', now() - interval '8 days')
	AND hw_c('a1', now() - interval '1 day') AND hw_c('a0', now() - interval '2 hours')
	AND hw_c('backdated', '2000-01-01') AND hw_c('after', now()) AS committed;
ALTER DATABASE regress_history_window_f SET prolly.history_window = '3 days';
\c regress_history_window_f
SELECT p.kind, l.message FROM prolly_history_plan(NULL) p
	JOIN dolt_log('main') l ON l.commit_hash = p.commit_hash
WHERE p.kind IN ('severed', 'boundary') ORDER BY 1, 2;
SELECT hw_c('future', '2100-01-01') AND hw_c('after future', now()) AS committed;
SELECT p.kind, l.message FROM prolly_history_plan(NULL) p
	JOIN dolt_log('main') l ON l.commit_hash = p.commit_hash
WHERE p.kind IN ('severed', 'boundary') ORDER BY 1, 2;
-- (a cut given past now severs it: its date counts as now)
SELECT p.kind, l.message FROM prolly_history_plan(now() + interval '1 year') p
	JOIN dolt_log('main') l ON l.commit_hash = p.commit_hash
WHERE p.kind IN ('severed', 'boundary') ORDER BY 1, 2;
SELECT dolt_checkout('-b', 'imported', (SELECT commit_hash FROM dolt_log('main') WHERE message = 'a10')) IS NOT NULL AS branched;
INSERT INTO hw.t VALUES (2, 'imported 1');
SELECT length(dolt_commit('-Am', 'imported 1', '--date', '2010-01-01')::text) > 0 AS committed;
UPDATE hw.t SET v = 'imported 2' WHERE id = 2;
SELECT length(dolt_commit('-Am', 'imported 2', '--date', '2010-01-02')::text) > 0 AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS back;
SELECT (dolt_merge('imported', '--no-ff', '-m', 'merged in'))[4] IS NOT NULL AS merged;
SELECT dolt_branch('-D', 'imported') IS NOT NULL AS dropped;
SELECT p.kind, l.message FROM prolly_history_plan(NULL) p
	JOIN dolt_log('main') l ON l.commit_hash = p.commit_hash
WHERE p.kind IN ('severed', 'boundary') ORDER BY 1, 2;
\c regress_history_window
DROP DATABASE regress_history_window_f;

-- A commit dated far ahead ages from when it arrived here, the head log's
-- first move to it, with a warning: once the window is past that, it is
-- severed like any other, the first plan to see it included.
CREATE DATABASE regress_history_window_g;
\c regress_history_window_g
CREATE SCHEMA hw VERSIONED;
CREATE TABLE hw.t (id int PRIMARY KEY, v text);
INSERT INTO hw.t VALUES (1, 'start');
SELECT length(dolt_commit('-Am', 'start', '--date', '2020-01-01')::text) > 0 AS committed;
UPDATE hw.t SET v = 'ahead';
SELECT length(dolt_commit('-am', 'ahead', '--date', '2099-01-01')::text) > 0 AS committed;
UPDATE hw.t SET v = 'now';
SELECT length(dolt_commit('-am', 'now')::text) > 0 AS committed;
ALTER DATABASE regress_history_window_g SET prolly.history_window = '1 second';
ALTER DATABASE regress_history_window_g SET prolly.history_window_action = delete;
SELECT pg_sleep(1.5);
\c regress_history_window_g
\set VERBOSITY terse
-- (the window evaluated for others, quietly: plan() and sever() warn)
SELECT l.message, prolly_history_in_window('main', l.commit_hash) AS in_window
FROM dolt_log('main') l ORDER BY l.commit_order;
SELECT p.kind, l.message FROM prolly_history_plan(NULL) p
	JOIN dolt_log('main') l ON l.commit_hash = p.commit_hash
WHERE p.kind IN ('severed', 'boundary') ORDER BY 1, 2;
SELECT fragment, commits FROM dolt_history_sever();
-- (nothing more past the window)
SELECT count(*) FROM prolly_history_plan(NULL) WHERE kind = 'severed';
\c regress_history_window
DROP DATABASE regress_history_window_g;

\c :home
DROP DATABASE regress_history_window;
DROP ROLE regress_hw_other;
