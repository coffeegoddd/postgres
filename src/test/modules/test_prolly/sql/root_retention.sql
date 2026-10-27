-- Which commits keep their page roots: every commit in the history window
-- (the database's settings, or a branch's own), none outside it, a tag's
-- commit's while the tag names it, and, under prolly.history_root_max_size,
-- the oldest in the window let go of first.  A move to a commit whose root
-- was let go of restores the nearest root before it and writes the rows
-- between.

SELECT current_database() AS home \gset
CREATE DATABASE root_retention;
ALTER DATABASE root_retention SET prolly.restore_min_changes = 0;
\c root_retention
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM pg_stat_prolly
	WHERE name IN ('apply_rows', 'relations_restored')
$$;
CREATE FUNCTION since(before jsonb) RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT key, value::bigint - (before ->> key)::bigint
	FROM jsonb_each_text(counted()) ORDER BY 1
$$;
-- the commits main keeps roots of, by their messages, newest first
CREATE FUNCTION kept() RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(l.message, ' ' ORDER BY substr(l.message, 2)::int DESC)
	FROM test_kept_commits('main') k JOIN dolt_log l ON l.commit_hash = k.commit_id
$$;

CREATE SCHEMA rr VERSIONED;
CREATE TABLE rr.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL);
INSERT INTO rr.t SELECT g, 0, repeat('w', 200) FROM generate_series(1, 4000) g;
CREATE FUNCTION digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM rr.t t
$$;
SELECT dolt_commit('-Am', 'c0') IS NOT NULL AS committed;
CREATE TEMP TABLE digests (n int, d text);
INSERT INTO digests VALUES (0, digest());
DO $$
BEGIN
	FOR i IN 1..12 LOOP
		UPDATE rr.t SET v = i WHERE id % 10 = i % 10;
		PERFORM dolt_commit('-am', 'c' || i);
		INSERT INTO digests VALUES (i, digest());
	END LOOP;
END $$;

-- with no window, the whole history is in it: every commit keeps its root
SELECT test_retain_roots('main');
SELECT kept();

-- a tag at c4, then a window of the newest five commits: exactly those
-- keep their roots, and the tag's commit
SELECT dolt_tag('keep4', (SELECT commit_hash FROM dolt_log WHERE message = 'c4'));
ALTER DATABASE root_retention SET prolly.history_min_commits = 5;
SELECT test_retain_roots('main');
SELECT kept();

-- a reset to c5, whose root is gone: the nearest root before it (c4's,
-- the tag's) is restored, and only the rows c5 changed written
SELECT dolt_hashof('HEAD') AS c12 \gset
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', (SELECT commit_hash FROM dolt_log WHERE message = 'c5'));
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'apply_rows' THEN value ELSE (value > 0)::int END
FROM since(:'before');
SELECT digest() = (SELECT d FROM digests WHERE n = 5) AS at_c5;

-- back at c12 by its root, and the tag deleted: c4's root goes, and c5's
-- kept by the reset, outside c12's window
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'c12');
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'apply_rows' THEN value ELSE (value > 0)::int END
FROM since(:'before');
SELECT digest() = (SELECT d FROM digests WHERE n = 12) AS at_c12;
SELECT dolt_tag('-d', 'keep4');
SELECT test_retain_roots('main');
SELECT kept();

-- a cap a byte under what the roots hold: the oldest one goes
ALTER DATABASE root_retention RESET prolly.history_min_commits;
CREATE FUNCTION held() RETURNS bigint LANGUAGE sql AS $$
	SELECT ('x' || encode(substring(test_refs_value('Lmain') FROM 9 FOR 8), 'hex'))::bit(64)::bigint
$$;
SELECT held() > 0 AS holds;
SELECT (held() - 1)::text AS cap \gset
ALTER DATABASE root_retention SET prolly.history_root_max_size = :'cap';
SELECT test_retain_roots('main');
SELECT kept();
SELECT held() <= :cap AS under_cap;

-- a cap of a byte: all but the newest go, and a tag's
SELECT dolt_tag('keep10', (SELECT commit_hash FROM dolt_log WHERE message = 'c10'));
ALTER DATABASE root_retention SET prolly.history_root_max_size = '1';
SELECT test_retain_roots('main');
SELECT kept();

-- with no cap, a commit keeps its root again
ALTER DATABASE root_retention RESET prolly.history_root_max_size;
UPDATE rr.t SET v = 13 WHERE id % 10 = 3;
SELECT dolt_commit('-am', 'c13') IS NOT NULL AS committed;
SELECT kept();

-- a commit made on the same parent as another branch's head leaves that
-- head's root: it is in that branch's window
SELECT dolt_hashof('HEAD') AS c13 \gset
SELECT dolt_branch('b13');
SELECT dolt_reset('--hard', 'HEAD~1');
UPDATE rr.t SET v = 14 WHERE id % 10 = 4;
SELECT dolt_commit('-am', 'c14') IS NOT NULL AS committed;
ALTER DATABASE root_retention SET prolly.history_min_commits = 2;
SELECT test_retain_roots('main');
SELECT kept();
SELECT EXISTS (SELECT FROM test_kept_commits('main') WHERE commit_id = :'c13')
	AS b13_head_kept;
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_checkout('b13');
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'apply_rows' THEN value ELSE (value > 0)::int END
FROM since(:'before');
SELECT dolt_checkout('main');

\c :home
DROP DATABASE root_retention;
