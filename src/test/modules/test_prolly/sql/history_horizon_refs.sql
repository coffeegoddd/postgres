-- What a sever keeps: every commit any ref names keeps its window, as a
-- branch's head does (a tag's, a remote-tracking branch's, a session's
-- detached head, a stash's, a merge's in progress, the commits a branch's
-- pages are kept as of), and every merge base of a branch with any ref,
-- every one where histories cross, with the paths to them.  A sever cuts
-- only what is past the window on every path from every ref.

\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/history_horizon_refs'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset
SELECT current_database() AS home \gset
\set VERBOSITY terse
CREATE DATABASE regress_history_refs;
\c regress_history_refs

CREATE FUNCTION hr_err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g');
END $$;
CREATE TABLE hr_names (hash text PRIMARY KEY, name text);
CREATE FUNCTION hr_commit(msg text, at timestamptz) RETURNS text LANGUAGE sql AS $$
	INSERT INTO hr_names
	SELECT trim(both '{}' from dolt_commit('-Am', msg, '--date', at::text)::text), msg
	RETURNING hash
$$;
-- readable(names): which of the named commits still read here
CREATE FUNCTION hr_readable(VARIADIC names text[]) RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(name || '=' || (hr_err(format('SELECT prolly_root_hash(%L)', hash)) = 'no error'),
					  ' ' ORDER BY name)
	FROM hr_names WHERE name = ANY (names)
$$;

CREATE SCHEMA hr VERSIONED;
CREATE TABLE hr.t (id int PRIMARY KEY, v text);
INSERT INTO hr.t SELECT g, 'v' FROM generate_series(1, 100) g;
SELECT length(hr_commit('old1', '2020-01-01')) > 0 AS committed;
UPDATE hr.t SET v = 'old2' WHERE id < 50;
SELECT length(hr_commit('old2', '2020-01-02')) > 0 AS committed;
UPDATE hr.t SET v = 'old3' WHERE id < 25;
SELECT length(hr_commit('old3', '2020-01-03')) > 0 AS committed;
UPDATE hr.t SET v = 'old4' WHERE id < 20;
SELECT length(hr_commit('old4', '2020-01-04')) > 0 AS committed;
UPDATE hr.t SET v = 'old5' WHERE id < 15;
SELECT length(hr_commit('old5', '2020-01-05')) > 0 AS committed;

-- Lineages only one ref reaches, each with a commit made just now:
-- a tag's,
SELECT dolt_checkout('-b', 'for_tag');
UPDATE hr.t SET v = 'tag' WHERE id = 1;
SELECT length(hr_commit('tagged', now())) > 0 AS committed;
SELECT dolt_tag('tg', 'for_tag');
-- a remote-tracking branch's,
SELECT dolt_checkout('-b', 'for_remote', 'main');
UPDATE hr.t SET v = 'remote' WHERE id = 2;
SELECT length(hr_commit('remote', now())) > 0 AS committed;
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'for_remote') AS pushed;
-- a stash's,
SELECT dolt_checkout('-b', 'for_stash', 'main');
UPDATE hr.t SET v = 'stashbase' WHERE id = 3;
SELECT length(hr_commit('stashbase', now())) > 0 AS committed;
UPDATE hr.t SET v = 'stashed' WHERE id = 3;
SELECT dolt_stash('push', 'for_stash') IS NOT NULL AS stashed;
-- a merge in progress's (theirs),
SELECT dolt_checkout('-b', 'merging', 'main');
UPDATE hr.t SET v = 'ours' WHERE id = 4;
SELECT length(hr_commit('ours', now())) > 0 AS committed;
SELECT dolt_checkout('-b', 'for_merge', 'main');
UPDATE hr.t SET v = 'theirs' WHERE id = 4;
SELECT length(hr_commit('theirs', now())) > 0 AS committed;
SELECT dolt_checkout('merging');
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('for_merge'))[3] AS conflicts;
-- and old history only a detached session reaches.
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'for_tag');
SELECT dolt_branch('-D', 'for_remote');
SELECT dolt_branch('-D', 'for_stash');
SELECT dolt_branch('-D', 'for_merge');

-- Criss-crossed history, old: two merge bases of side_a and side_b.
SELECT dolt_checkout('-b', 'side_a', 'main');
UPDATE hr.t SET v = 'xa' WHERE id = 10;
SELECT length(hr_commit('xa', '2020-01-06')) > 0 AS committed;
SELECT dolt_checkout('-b', 'side_b', 'main');
UPDATE hr.t SET v = 'xb' WHERE id = 11;
SELECT length(hr_commit('xb', '2020-01-06 01:00')) > 0 AS committed;
SELECT dolt_checkout('side_a');
SELECT (dolt_merge('side_b', '--no-ff', '-m', 'xab'))[4] AS merged;
UPDATE hr_names SET name = 'xab' WHERE hash = dolt_hashof('side_a');
INSERT INTO hr_names VALUES (dolt_hashof('side_a'), 'xab') ON CONFLICT DO NOTHING;
SELECT dolt_checkout('side_b');
SELECT (dolt_merge((SELECT hash FROM hr_names WHERE name = 'xa'), '--no-ff', '-m', 'xba'))[4] AS merged;
INSERT INTO hr_names VALUES (dolt_hashof('side_b'), 'xba') ON CONFLICT DO NOTHING;
SELECT dolt_checkout('main');

-- A sever before thirty days ago: what is past every ref's window.
SELECT kind, coalesce(n.name, p.commit_hash) AS commit, coalesce(m.name, p.parent_hash) AS parent, why
FROM prolly_history_plan(now() - interval '30 days') p
LEFT JOIN hr_names n ON n.hash = p.commit_hash LEFT JOIN hr_names m ON m.hash = p.parent_hash
ORDER BY 1, 2, 3;
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
SELECT commits FROM dolt_history_drop(now() - interval '30 days');
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
SELECT hr_readable('tagged', 'remote', 'stashbase', 'ours', 'theirs') AS refs_kept;
SELECT hr_readable('xa', 'xb') AS both_bases_kept;

-- A detached checkout's old commit keeps it, too, kept once left.
SELECT (dolt_checkout((SELECT hash FROM hr_names WHERE name = 'old4')))[1] AS detached;
SELECT (dolt_checkout('main'))[1] AS back;
SELECT count(*) AS detached_kept FROM pg_branch WHERE brname LIKE 'detached/%';
SELECT commits FROM dolt_history_drop(now() - interval '30 days');
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
SELECT hr_readable('old4') AS detached_kept;

-- The tag, the merge's and the stash's lineages merge and diff still.
SELECT count(*) > 0 AS tag_diff FROM dolt_diff('main', 'tg', NULL::hr.t);

\c :home
DROP DATABASE regress_history_refs;
