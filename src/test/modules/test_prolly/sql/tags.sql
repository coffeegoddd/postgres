-- Tags: names for commits, made and deleted with dolt_tag, listed by
-- dolt_tags, read wherever a revision is, kept alive by collection, and
-- carried by push, fetch and clone.

-- (a database of its own: its history is the test's alone)
CREATE DATABASE regress_tags;
\c regress_tags
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SET dolt.author = 'Tagger <tagger@example.com>';

SELECT current_database() AS db \gset
\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/tags'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset

CREATE SCHEMA tg VERSIONED;
CREATE TABLE tg.t (id int PRIMARY KEY, v text);
INSERT INTO tg.t VALUES (0, 'a'), (1, 'a'), (2, 'a');
SELECT dolt_commit('-Am', 'created table') IS NOT NULL AS committed;
SELECT dolt_hashof('HEAD') AS c1 \gset

-- made at HEAD, or at a ref; a name taken is refused, as Dolt refuses it
SELECT dolt_tag('v1');
SELECT dolt_tag('v1', 'HEAD');
SELECT dolt_tag('v1');
SELECT tag_name, tag_hash = :'c1' AS at_c1, tagger, email, date IS NOT NULL AS dated,
	message FROM dolt_tags;
DELETE FROM tg.t WHERE id = 0;
INSERT INTO tg.t VALUES (3, 'b');
SELECT dolt_commit('-am', 'made changes') IS NOT NULL AS committed;
SELECT dolt_hashof('HEAD') AS c2 \gset
SELECT dolt_tag('v2', '-m', 'create tag v2');
SELECT dolt_tag('-m', 'at the first', 'v0', 'HEAD~1');
SELECT dolt_tag('--author', 'Other <o@example.com>', 'v3', :'c1');
SELECT tag_name, tag_hash = :'c2' AS at_c2, tagger, message FROM dolt_tags ORDER BY 1;

-- refused: no name, a listing, too many, a bad name, a ref that isn't one
SELECT dolt_tag('-v');
SELECT dolt_tag('-m', 'only a message');
SELECT dolt_tag('v4', 'HEAD', 'extra');
SELECT dolt_tag('bad..name');
SELECT dolt_tag('HEAD');
DO $$
BEGIN
	PERFORM dolt_tag(dolt_hashof('HEAD'));
EXCEPTION WHEN invalid_name THEN
	RAISE NOTICE 'a commit''s id refused: %', (SELECT regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>'));
END $$;
SELECT dolt_tag('v4', 'nowhere');
SELECT dolt_tag('-d', '-m', 'x', 'v3');

-- a tag is a revision: hashof, ancestry, log, diff, as of
SELECT dolt_hashof('v1') = :'c1' AS v1, dolt_hashof('refs/tags/v2') = :'c2' AS v2,
	dolt_hashof('v2~1') = :'c1' AS v2_parent, dolt_hashof('v2^') = :'c1' AS v2_caret;
SELECT message FROM dolt_log('v1');
SELECT to_table_name, diff_type FROM dolt_diff_summary('v1', 'v2');
SELECT * FROM dolt_at('v1', NULL::tg.t) ORDER BY id;
SELECT dolt_merge_base('v1', 'v2') = :'c1' AS base;

-- a branch and a tag may share a name; the branch is read first, as in
-- Dolt, and refs/tags/ names the tag
SELECT dolt_branch('both', 'v1');
SELECT dolt_tag('both', 'v2');
INSERT INTO tg.t VALUES (9, 'x');
SELECT dolt_commit('-am', 'moved on') IS NOT NULL AS committed;
SELECT dolt_hashof('both') = :'c1' AS branch_first,
	dolt_hashof('refs/tags/both') = :'c2' AS tag_by_ref,
	dolt_hashof('refs/heads/both') = :'c1' AS branch_by_ref;
SELECT dolt_hashof('refs/heads/v1');
SELECT dolt_branch('-D', 'both');
SELECT dolt_tag('-d', 'both');

-- a branch made at a tag; a merge and a reset of one
SELECT dolt_checkout('-b', 'other', 'v1') IS NOT NULL AS on_other;
INSERT INTO tg.t VALUES (8, 'o');
SELECT dolt_commit('-am', 'in other') IS NOT NULL AS committed;
SELECT dolt_merge('v2') IS NOT NULL AS merged;
SELECT id FROM tg.t ORDER BY id;
SELECT dolt_reset('--hard', 'v1');
SELECT id FROM tg.t ORDER BY id;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'other');

-- checking out a tag: a detached session at its commit, read-only
SELECT (dolt_checkout('v1'))[2] = 'HEAD is now at ' || :'c1' || ' (detached, read-only)'
	AS detached;
SELECT active_branch() = 'detached/' || :'c1' AS on_its_commit;
SELECT id FROM tg.t ORDER BY id;
INSERT INTO tg.t VALUES (99, 'no');
-- (a tag changes only the refs: made there, as a branch is, and deleted)
SELECT dolt_tag('v1.1');
SELECT dolt_hashof('v1.1') = :'c1' AS tagged_here;
SELECT dolt_tag('-d', 'v1.1');
-- (a tag by its full ref name checks out as by its name)
SELECT (dolt_checkout('refs/tags/v2'))[2] = 'HEAD is now at ' || :'c2' || ' (detached, read-only)'
	AS detached_by_ref;
\c :db
SET dolt.author = 'Tagger <tagger@example.com>';

-- deleted, one or several; one that isn't is refused, and nothing deleted
SELECT dolt_tag('-d', 'v3', 'nowhere');
SELECT tag_name FROM dolt_tags ORDER BY 1;
SELECT dolt_tag('-d', 'v0', 'v3');
SELECT tag_name FROM dolt_tags ORDER BY 1;
-- with the transaction
BEGIN;
SELECT dolt_tag('gone');
ROLLBACK;
SELECT tag_name FROM dolt_tags ORDER BY 1;

-- the reflog names tags by their full names
SELECT ref, commit_hash = :'c2' AS at_c2 FROM dolt_reflog('refs/tags/v2');

-- a tag keeps its commit alive: a branch's commits only it reaches survive
-- a collection
SELECT dolt_checkout('-b', 'doomed') IS NOT NULL AS on_doomed;
INSERT INTO tg.t VALUES (50, 'kept');
SELECT dolt_commit('-am', 'only tagged') IS NOT NULL AS committed;
SELECT dolt_hashof('HEAD') AS c3 \gset
SELECT dolt_tag('keep');
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'doomed');
-- (and a commit only a deleted branch reached, for contrast: collected)
SELECT dolt_checkout('-b', 'gone') IS NOT NULL AS on_gone;
INSERT INTO tg.t VALUES (51, 'gone');
SELECT dolt_commit('-am', 'not tagged') IS NOT NULL AS committed;
SELECT dolt_hashof('HEAD') AS c4 \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'gone');
SELECT kept > 0 AS kept, freed > 0 AS freed FROM test_chunk_gc(true);
SELECT test_chunk_has(:'c3') AS tagged_kept, test_chunk_has(:'c4') AS untagged_kept;
SELECT dolt_hashof('keep') = :'c3' AS kept;
SELECT id, v FROM dolt_at('keep', NULL::tg.t) WHERE id = 50;
SELECT message FROM dolt_log('keep') LIMIT 1;

-- pushed to a remote, by name or full name; fetched and cloned with it
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main') IS NOT NULL AS pushed;
SELECT dolt_push('origin', 'v2');
SELECT dolt_push('origin', 'v2');
SELECT dolt_push('origin', 'refs/tags/keep');
SELECT dolt_push('-u', 'origin', 'v1');
SELECT dolt_push('origin', 'refs/tags/nowhere');
-- a tag is of the remote's one canonical format, as a head is: pushed
-- first, it records the format; pushed to a remote of another, refused
SELECT :'url' || '-tagfirst' AS url_tf \gset
SELECT dolt_remote('add', 'tagfirst', :'url_tf');
SELECT dolt_push('tagfirst', 'v2') IS NOT NULL AS pushed;
SELECT set_config('tags.url_tf', :'url_tf', false) IS NOT NULL AS set;
DO $$
BEGIN
	PERFORM test_remote_set_head(current_setting('tags.url_tf'), 'main',
								 dolt_hashof('v2'), '1');
	RAISE NOTICE 'a head of format 1 taken';
EXCEPTION WHEN feature_not_supported THEN
	RAISE NOTICE 'a head of format 1 refused';
END $$;
SELECT :'url' || '-otherformat' AS url_of \gset
SELECT test_remote_set_head(:'url_of', 'main', dolt_hashof('v2'), '1');
SELECT dolt_remote('add', 'otherformat', :'url_of');
DO $$
BEGIN
	PERFORM dolt_push('otherformat', 'v2');
	RAISE NOTICE 'a tag pushed to a remote of format 1';
EXCEPTION WHEN feature_not_supported THEN
	RAISE NOTICE 'a tag refused by a remote of format 1';
END $$;

CREATE DATABASE regress_tags_b;
\c regress_tags_b
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_fetch();
-- (keep's commit is on no branch fetched: a fetch follows only tags of
-- commits it has, as Dolt's does)
SELECT tag_name, tag_hash = :'c2' AS at_c2, tagger, message FROM dolt_tags ORDER BY 1;
SELECT dolt_fetch('origin', 'main');
SELECT dolt_hashof('v2') = :'c2' AS resolves;
\c :db
SELECT dolt_tag('-d', 'v2');
SELECT dolt_tag('v2', 'v1', '-m', 'moved');
SELECT dolt_push('origin', 'v2') IS NOT NULL AS pushed;
\c regress_tags_b
-- a tag fetched again takes the remote's, as Dolt's fetch sets it
SELECT dolt_fetch();
SELECT tag_name, tag_hash = :'c1' AS at_c1, message FROM dolt_tags ORDER BY 1;
\c :db
DROP DATABASE regress_tags_b;

CREATE DATABASE regress_tags_c;
\c regress_tags_c
SELECT dolt_clone(:'url');
SELECT tag_name, tag_hash = :'c1' AS at_c1, message FROM dolt_tags ORDER BY 1;
SELECT dolt_hashof('v2') = :'c1' AS resolves;
\c :db
DROP DATABASE regress_tags_c;

-- refs and remotes of an older format are refused, not read: refs without
-- their format, as an older build's, and a version 1 manifest
CREATE DATABASE regress_tags_old;
\c regress_tags_old
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT test_chunk_move_root(test_chunk_root(), test_chunk_declaring_tree('{}'));
SELECT name FROM dolt_branches;
SELECT dolt_tag('v1');
-- (and refs of the format before this build's)
SELECT test_chunk_move_root(test_chunk_root(),
							test_chunk_items_tree('{!}', '{14}'));
SELECT name FROM dolt_branches;
\c :db
DROP DATABASE regress_tags_old;
SELECT :'dir' || '/old-' || txid_current() AS olddir \gset
\set oldmanifest :olddir '/prolly_remote'
SELECT dolt_remote('add', 'oldremote', 'file://' || :'olddir');
SELECT dolt_push('oldremote', 'main') IS NOT NULL AS pushed;
COPY (SELECT 'prolly remote 1') TO :'oldmanifest';
DO $$
BEGIN
	PERFORM dolt_fetch('oldremote');
EXCEPTION WHEN feature_not_supported THEN
	RAISE NOTICE 'refused: %', regexp_replace(SQLERRM, '"/[^"]*"', '"<dir>"');
END $$;
\c postgres
DROP DATABASE regress_tags;
