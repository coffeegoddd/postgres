-- Remotes: a database's commits pushed to a file remote, fetched into
-- another database and pulled back.  Only the chunks the other side lacks
-- move; what is received is checked (each chunk against its hash, every
-- reference resolved, every root of this build's format) before the store
-- takes any of it.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
-- (this build's canonical format, for a remote's head set by hand)
SELECT value AS fmt FROM test_fold_items() WHERE key = '!:format' \gset

SELECT current_database() AS db \gset
\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/remotes'
SELECT 'file://' || :'dir' || '/' || txid_current() AS run \gset
\set url_a :run '/a'
\set url_bad :run '/bad'
\set url_old :run '/old'

-- err(q): q's error, its hashes and paths masked
CREATE FUNCTION pg_temp.err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g'),
						  '"/[^"]*"', '"<dir>"', 'g');
END $$;

CREATE SCHEMA rm VERSIONED;
CREATE TABLE rm.t (id int PRIMARY KEY, v text);
INSERT INTO rm.t SELECT g, 'v' || g FROM generate_series(1, 500) g;
SELECT length(dolt_commit('-Am', 'rm 1')::text) AS committed;
-- (commits sharing most of their trees: a fetch meets chunks it received
-- already between ones it hasn't)
UPDATE rm.t SET v = 'w' WHERE id = 1;
SELECT length(dolt_commit('-am', 'rm 2')::text) AS committed;
UPDATE rm.t SET v = 'w' WHERE id = 500;
SELECT length(dolt_commit('-am', 'rm 3')::text) AS committed;

-- remotes are named, by URL
SELECT dolt_remote('add', 'origin', :'url_a');
SELECT name, url = :'url_a' AS url_is_a FROM dolt_remotes;
SELECT dolt_remote('add', 'origin', :'url_a');
SELECT dolt_remote('add', 'a/b', :'url_a');
SELECT dolt_remote('add', 'other', 'ftp://example.com/x');
SELECT dolt_remote('add', 'other', 'file://relative/path');
SELECT dolt_remote('add', 'other', 'nothing');
SELECT dolt_fetch('nowhere');

-- a push makes the remote, sends the branch and moves its head there
SELECT dolt_push('origin', 'main') AS pushed;
SELECT name, hash = dolt_hashof('main') AS at_main FROM dolt_remote_branches;
SELECT dolt_push('origin', 'main') AS again;
SELECT dolt_hashof('origin/main') = dolt_hashof('main') AS resolves,
	dolt_hashof('remotes/origin/main') = dolt_hashof('main') AS resolves_long;

-- another database fetches it: the rows read back there
CREATE DATABASE regress_remotes_b;
\c regress_remotes_b
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT dolt_remote('add', 'origin', :'url_a');
SELECT dolt_fetch();
SELECT name FROM dolt_remote_branches;
SELECT dolt_branch('work', 'origin/main');
SELECT dolt_checkout('work') IS NOT NULL AS on_work;
SELECT count(*), max(v) FROM rm.t;

-- a commit there pushed as a new branch
UPDATE rm.t SET v = 'b' WHERE id <= 5;
SELECT length(dolt_commit('-am', 'from b')::text) AS committed;
SELECT dolt_push('origin', 'work') AS pushed;
\c :db

-- pulled into main: a fast-forward
SELECT dolt_pull('origin', 'work') IS NOT NULL AS pulled;
SELECT count(*) FILTER (WHERE v = 'b') AS from_b FROM rm.t;
SELECT dolt_hashof('main') = dolt_hashof('origin/work') AS fast_forwarded;

-- a push that isn't a fast-forward is refused, unless forced
SELECT dolt_push('origin', 'main') AS pushed;
SELECT dolt_hashof('main') AS ahead \gset
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_push('origin', 'main');
SELECT dolt_push('--force', 'origin', 'main') AS forced;
SELECT dolt_reset('--hard', :'ahead');

-- upstreams: a push or pull without a remote uses the branch's
SELECT dolt_push();
SELECT dolt_push('--set-upstream', 'origin', 'main') AS pushed;
SELECT dolt_push() AS pushed_upstream;
SELECT dolt_pull() IS NOT NULL AS pulled_upstream;

-- a push to an upstream of another name: the session's branch, to it
SELECT dolt_checkout('-b', 'feature') IS NOT NULL AS on_feature;
UPDATE rm.t SET v = 'f' WHERE id = 2;
SELECT length(dolt_commit('-am', 'feature')::text) AS committed;
SELECT test_refs_set_upstream('feature', 'origin', 'feat');
SELECT dolt_push() AS pushed;
SELECT dolt_hashof('origin/feat') = dolt_hashof('feature') AS to_feat;
SELECT dolt_pull('origin', 'nowhere');
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'feature');
SELECT dolt_pull('origin', 'feat') IS NOT NULL AS pulled;

-- a delete without -D: of a branch merged into its upstream (its
-- remote-tracking branch, as last fetched), as in Dolt and git; into the
-- session's branch only when it has no upstream there
SELECT dolt_checkout('-b', 'up3') IS NOT NULL AS on_up3;
UPDATE rm.t SET v = 'u' WHERE id = 3;
SELECT length(dolt_commit('-am', 'up3')::text) AS committed;
SELECT dolt_push('-u', 'origin', 'up3') AS pushed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-d', 'up3');
SELECT count(*) AS up3_left FROM dolt_branches WHERE name = 'up3';
-- (merged into main, but ahead of its upstream: refused)
SELECT dolt_checkout('-b', 'up4') IS NOT NULL AS on_up4;
SELECT dolt_push('-u', 'origin', 'up4') AS pushed;
UPDATE rm.t SET v = 'u' WHERE id = 4;
SELECT length(dolt_commit('-am', 'up4')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_merge('up4') IS NOT NULL AS merged;
SELECT dolt_branch('-d', 'up4');
SELECT dolt_branch('-D', 'up4');
-- (an upstream not fetched: into the session's branch)
SELECT dolt_branch('up5');
SELECT test_refs_set_upstream('up5', 'origin', 'nowhere');
SELECT dolt_branch('-d', 'up5');
SELECT dolt_branch('up6');
SELECT test_refs_set_upstream('up6', 'origin', 'nowhere');
SELECT dolt_checkout('-b', 'ahead6') IS NOT NULL AS on_ahead6;
UPDATE rm.t SET v = 'u' WHERE id = 6;
SELECT length(dolt_commit('-am', 'ahead6')::text) AS committed;
SELECT dolt_checkout('up6') IS NOT NULL AS on_up6;
SELECT dolt_merge('ahead6') IS NOT NULL AS merged;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-d', 'up6');
SELECT dolt_branch('-D', 'up6');
SELECT dolt_branch('-D', 'ahead6');
-- (the session's own branch, ahead of its upstream: refused as checked
-- out, before the merge test, as in Dolt and git, so not with a -D hint
-- that -D doesn't get past)
SELECT dolt_checkout('-b', 'cur') IS NOT NULL AS on_cur;
SELECT dolt_push('-u', 'origin', 'cur') AS pushed;
INSERT INTO rm.t VALUES (9070, 'cur');
SELECT length(dolt_commit('-am', 'cur')::text) AS committed;
SELECT dolt_branch('-d', 'cur');
SELECT dolt_branch('-D', 'cur');
-- (main, ahead of its upstream: refused as main, from another branch too)
SELECT dolt_hashof('main') <> dolt_hashof('origin/main') AS main_ahead;
SELECT dolt_branch('-d', 'main');
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-d', 'main');
SELECT dolt_branch('-D', 'cur');
-- (a branch deleted keeps its upstream, as in Dolt, whose delete leaves the
-- branch's config: one made again of its name has it, and -d checks it
-- against the old branch's remote-tracking branch; git drops it)
SELECT dolt_branch('rc', 'HEAD~1');
SELECT dolt_push('-u', 'origin', 'rc') AS pushed;
SELECT dolt_branch('-D', 'rc');
SELECT dolt_branch('rc');
SELECT dolt_branch('-d', 'rc');
SELECT dolt_branch('-D', 'rc');

-- a walk from a commit: a chunk that isn't a commit visits nothing, one
-- the store hasn't is an error (its store isn't whole)
SELECT test_walk_commit_count(dolt_hashof('main')) > 0 AS walks_a_commit;
SELECT test_walk_commit_count(tree) AS not_a_commit
  FROM test_fold_trees() WHERE name = 'rm.t';
SELECT test_walk_commit_count(test_content_hash('no such commit'::bytea));

-- a long history pushed and fetched: walked a commit at a time, each
-- once, however long
SELECT dolt_checkout('-b', 'long') IS NOT NULL AS on_long;
SELECT test_commit_chain(dolt_hashof('HEAD'), 50000) AS long_head \gset
SELECT test_refs_set_head(:'long_head');
SELECT dolt_push('origin', 'long') AS pushed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'long');
\c regress_remotes_b
SELECT dolt_fetch('origin', 'long');
SELECT dolt_hashof('origin/long') = :'long_head' AS fetched_long;
\c :db

-- a commit whose root is of an older format (no "!:format"): refused
SELECT test_root_without_format(test_fold_root()) AS oldroot \gset
SELECT test_commit_of(:'oldroot', 'old format') AS oldcommit \gset
SELECT test_remote_put(:'url_a', :'oldroot', test_chunk_get(:'oldroot'));
SELECT test_remote_put(:'url_a', :'oldcommit', test_chunk_get(:'oldcommit'));
SELECT test_remote_set_head(:'url_a', 'old', :'oldcommit', :'fmt');
\c regress_remotes_b
CREATE FUNCTION pg_temp.err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g'),
						  '"/[^"]*"', '"<dir>"', 'g');
END $$;
SELECT pg_temp.err($$SELECT dolt_fetch()$$);
SELECT count(*) AS old_tracked FROM dolt_remote_branches WHERE name LIKE '%/old';
SELECT dolt_fetch('origin', 'main');
\c :db

-- a remote's head the remote lacks chunks of: nothing is taken
SELECT dolt_remote('add', 'bad', :'url_bad');
SELECT test_chunk_get(dolt_hashof('main')) AS commit_bytes \gset
SELECT test_remote_put(:'url_bad', dolt_hashof('main'), :'commit_bytes');
SELECT test_content_hash('not a commit'::bytea) AS fake \gset
SELECT test_remote_set_head(:'url_bad', 'main', dolt_hashof('main'), :'fmt');

-- (fetched into a third database, which has none of the chunks)
CREATE DATABASE regress_remotes_c;
\c regress_remotes_c
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE FUNCTION pg_temp.err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g'),
						  '"/[^"]*"', '"<dir>"', 'g');
END $$;
SELECT dolt_remote('add', 'bad', :'url_bad');
SELECT pg_temp.err($$SELECT dolt_fetch('bad')$$);
SELECT count(*) AS tracking FROM dolt_remote_branches;
SELECT count(*) AS table_files FROM test_chunk_table_files();

-- a chunk that doesn't match its hash
SELECT dolt_remote('add', 'liar', :'url_bad' || '-liar');
SELECT test_remote_put(:'url_bad' || '-liar', :'fake', '\x01020304');
SELECT test_remote_set_head(:'url_bad' || '-liar', 'main', :'fake', :'fmt');
SELECT pg_temp.err($$SELECT dolt_fetch('liar')$$);

-- a remote of another canonical format
SELECT dolt_remote('add', 'old', :'url_old');
SELECT test_remote_set_head(:'url_old', 'main', :'fake', '1');
SELECT replace(pg_temp.err($$SELECT dolt_fetch('old')$$), 'not ' || :'fmt', 'not <this build''s>');
\c :db

-- a role without the privileges of pg_read_server_files can't read one,
-- nor push to one (a push reads it), whatever it may write
CREATE FUNCTION pg_temp.err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g'),
						  '"/[^"]*"', '"<dir>"', 'g');
END $$;
CREATE ROLE regress_remote_user;
GRANT ALL ON SCHEMA rm TO regress_remote_user;
SET ROLE regress_remote_user;
SELECT pg_temp.err($$SELECT dolt_fetch('origin')$$);
RESET ROLE;
GRANT pg_write_server_files TO regress_remote_user;
SET ROLE regress_remote_user;
SELECT pg_temp.err($$SELECT dolt_push('origin', 'main')$$);
RESET ROLE;
DROP OWNED BY regress_remote_user;
DROP ROLE regress_remote_user;

-- a remote's branch of any name's length
SELECT test_remote_set_head(:'url_a', repeat('x', 1500), dolt_hashof('main'), :'fmt');
SELECT dolt_fetch();
SELECT length(name) AS long_name FROM dolt_remote_branches WHERE name LIKE '%xxxx%';

-- pull with no upstream names pull in its hint
SELECT dolt_checkout('-b', 'loose') IS NOT NULL AS on_loose;
SELECT dolt_pull();
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'loose');

-- removing a remote removes its remote-tracking branches and upstreams
SELECT dolt_remote('remove', 'origin');
SELECT count(*) AS tracking FROM dolt_remote_branches;
SELECT dolt_push();
SELECT dolt_remote('remove', 'bad');
SELECT count(*) AS remotes FROM dolt_remotes;

DROP DATABASE regress_remotes_b;
DROP DATABASE regress_remotes_c;
DROP SCHEMA rm CASCADE;
SELECT length(dolt_commit('-Am', 'rm gone')::text) AS committed;
