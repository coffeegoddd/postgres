-- This database as another's remote: the functions a network remote calls.
-- Fetching takes owning the database or pg_prolly_fetch's privileges,
-- pushing pg_prolly_push's; a push's chunks are checked as a fetch's are
-- before anything is added, and its branch moved as a compare-and-swap.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT value AS fmt FROM test_fold_items() WHERE key = '!:format' \gset

CREATE FUNCTION pg_temp.h(ref text) RETURNS bytea LANGUAGE sql
  AS $$ SELECT decode(dolt_hashof(ref), 'hex') $$;

CREATE SCHEMA rs VERSIONED;
CREATE TABLE rs.t (id int PRIMARY KEY, v text);
INSERT INTO rs.t VALUES (1, 'one');
SELECT length(dolt_commit('-Am', 'one')::text) AS committed;
SELECT dolt_branch('rs_b');
INSERT INTO rs.t VALUES (2, 'two');
SELECT length(dolt_commit('-Am', 'two')::text) AS committed;

-- what a fetch asks
SELECT dolt_serve_format() = :'fmt' AS same_format;
SELECT branch, commit_id = pg_temp.h(branch) AS at_head FROM dolt_serve_heads()
  WHERE branch IN ('main', 'rs_b') ORDER BY branch;
SELECT dolt_serve_has(ARRAY[pg_temp.h('main'), '\x00ff'::bytea]);
SELECT hash = pg_temp.h('main') AS same_hash, length(chunk) > 0 AS has_bytes
  FROM dolt_serve_get(ARRAY[pg_temp.h('main'), '\x00ff'::bytea]);

-- what changed since known commits: a head's own chunks and what its root
-- changed, far fewer than all it reaches; nothing for a known head
SELECT (SELECT count(*) FROM dolt_serve_since(ARRAY[pg_temp.h('main')], ARRAY[pg_temp.h('main~1')]))
	 < (SELECT count(*) FROM dolt_serve_since(ARRAY[pg_temp.h('main')], '{}')) AS fewer,
	   (SELECT count(*) FROM dolt_serve_since(ARRAY[pg_temp.h('main')], ARRAY[pg_temp.h('main~1')])) > 0 AS some,
	   (SELECT count(*) FROM dolt_serve_since(ARRAY[pg_temp.h('main')], ARRAY[pg_temp.h('main')])) AS none;

-- a push's move: a compare-and-swap of the branch's head
SELECT dolt_serve_move_head('rs_b', pg_temp.h('main'), pg_temp.h('main'), :'fmt', false);	-- not there
SELECT dolt_serve_move_head('rs_b', pg_temp.h('rs_b'), pg_temp.h('main'), :'fmt', false);
SELECT dolt_hashof('rs_b') = dolt_hashof('main') AS moved;
-- a branch it doesn't have: made, with only its head
SELECT dolt_serve_move_head('rs_new', '', pg_temp.h('HEAD~1'), :'fmt', false);
SELECT dolt_hashof('rs_new') = dolt_hashof('HEAD~1') AS made;
-- deleted
SELECT dolt_serve_move_head('rs_new', pg_temp.h('rs_new'), NULL, :'fmt', false);
SELECT count(*) AS left FROM dolt_branches WHERE name = 'rs_new';
-- deleted again: already gone, as asked
SELECT dolt_serve_move_head('rs_new', '', NULL, :'fmt', false) AS again;
-- another branch with uncommitted changes: its delete refused unless
-- forced, whether or not it is merged into this session's branch
SELECT dolt_checkout('-b', 'rs_dirty') IS NOT NULL AS on_dirty;
INSERT INTO rs.t VALUES (40, 'uncommitted');
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_serve_move_head('rs_dirty', pg_temp.h('rs_dirty'), NULL, :'fmt', false);
SELECT dolt_serve_move_head('rs_dirty', pg_temp.h('rs_dirty'), NULL, :'fmt', true) AS deleted;
SELECT count(*) AS left FROM dolt_branches WHERE name = 'rs_dirty';
-- the session's branch with uncommitted changes: refused unless forced
INSERT INTO rs.t VALUES (3, 'three');
SELECT dolt_serve_move_head('main', pg_temp.h('main'), pg_temp.h('HEAD~1'), :'fmt', false);
SELECT dolt_serve_move_head('main', pg_temp.h('main'), pg_temp.h('HEAD~1'), :'fmt', true);
SELECT id, v FROM rs.t ORDER BY id;		-- as the commit has them
SELECT * FROM dolt_status;
-- a commit needing what the database lacks: refused, as any move is
SELECT test_commit_needing(test_fold_root(), ARRAY['extension ltree'],
						   ARRAY['{1.3,public}']) AS lt \gset
SELECT dolt_serve_move_head('main', pg_temp.h('main'), decode(:'lt', 'hex'), :'fmt', false);
-- another format: refused
DO $$
BEGIN
	PERFORM dolt_serve_move_head('main', pg_temp.h('main'), pg_temp.h('main'), '0', false);
EXCEPTION WHEN feature_not_supported THEN
	RAISE NOTICE 'refused: %', regexp_replace(SQLERRM, 'format [0-9]+', 'format N');
END $$;

-- a chunk sent under a hash it doesn't have: refused when received, and
-- nothing moved
SELECT dolt_hashof('rs_b') AS rs_b \gset
BEGIN;
SELECT dolt_serve_receive(ARRAY['\x0102030405060708091011121314151617181920212223242526272829303132'::bytea],
						  ARRAY['\xdeadbeef'::bytea]);
SELECT dolt_serve_move_head('rs_b', pg_temp.h('rs_b'),
							'\x0102030405060708091011121314151617181920212223242526272829303132',
							:'fmt', false);
ROLLBACK;
SELECT dolt_hashof('rs_b') = :'rs_b' AS unmoved;

-- privileges: none without the roles; fetching with pg_prolly_fetch; and
-- pushing only with pg_prolly_push
CREATE ROLE regress_rs_none;
CREATE ROLE regress_rs_fetch IN ROLE pg_prolly_fetch;
CREATE ROLE regress_rs_push IN ROLE pg_prolly_push;
SET ROLE regress_rs_none;
SELECT dolt_serve_format();
RESET ROLE;
SET ROLE regress_rs_fetch;
SELECT dolt_serve_format() = :'fmt' AS fetches;
SELECT dolt_serve_move_head('rs_b', pg_temp.h('rs_b'), pg_temp.h('rs_b'), :'fmt', false);
RESET ROLE;
SET ROLE regress_rs_push;
SELECT count(*) > 0 AS sees_heads FROM dolt_serve_heads();
SELECT dolt_serve_move_head('rs_b', pg_temp.h('rs_b'), pg_temp.h('rs_b'), :'fmt', false) AS pushes;
RESET ROLE;
DROP ROLE regress_rs_none;
DROP ROLE regress_rs_fetch;
DROP ROLE regress_rs_push;

SELECT dolt_branch('-D', 'rs_b');
DROP SCHEMA rs CASCADE;
SELECT length(dolt_commit('-Am', 'rs gone')::text) AS committed;

-- what changed since known commits, past a merge: a receiver that has one
-- parent (and so the fork point beneath it) is sent the merge and the other
-- side's commit, not the history beneath them
CREATE SCHEMA rsm VERSIONED;
CREATE TABLE rsm.t (id int PRIMARY KEY, v text);
INSERT INTO rsm.t SELECT g, 'v' || g FROM generate_series(1, 20000) g;
SELECT (dolt_commit('-Am', 'rsm c1'))[1] AS rsm_c1 \gset
SELECT dolt_branch('rsm_f');
UPDATE rsm.t SET v = 'm' WHERE id = 1;
SELECT (dolt_commit('-Am', 'rsm m1'))[1] AS rsm_m1 \gset
SELECT dolt_checkout('rsm_f') IS NOT NULL AS on_f;
UPDATE rsm.t SET v = 'f' WHERE id = 20000;
SELECT (dolt_commit('-Am', 'rsm f1'))[1] AS rsm_f1 \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('rsm_f'))[3:4];
CREATE FUNCTION pg_temp.since(known text[]) RETURNS bigint LANGUAGE sql
  AS $$ SELECT count(*) FROM dolt_serve_since(ARRAY[pg_temp.h('main')],
		 ARRAY(SELECT decode(k, 'hex') FROM unnest(known) k)) $$;
SELECT pg_temp.since(ARRAY[:'rsm_m1']) * 4 < pg_temp.since('{}') AS far_fewer,
	   pg_temp.since(ARRAY[:'rsm_m1']) = pg_temp.since(ARRAY[:'rsm_m1', :'rsm_c1']) AS as_with_its_base,
	   pg_temp.since(ARRAY[:'rsm_f1']) * 4 < pg_temp.since('{}') AS other_side_too,
	   pg_temp.since(ARRAY[:'rsm_m1', :'rsm_f1']) < pg_temp.since(ARRAY[:'rsm_m1']) AS fewer_with_both,
	   pg_temp.since(ARRAY[repeat('ab', 32)]) = pg_temp.since('{}') AS unknown_ignored;
SELECT dolt_branch('-D', 'rsm_f');
DROP SCHEMA rsm CASCADE;
SELECT length(dolt_commit('-Am', 'rsm gone')::text) AS committed;

-- what changed since known commits, whatever the commits' times: a
-- commit's time is its transaction's start, the same for all one
-- transaction commits, and earlier than its parent's for one begun before
-- the parent was committed (here made by hand, with the earliest time)
CREATE SCHEMA rst VERSIONED;
CREATE TABLE rst.t (id int PRIMARY KEY, v text);
INSERT INTO rst.t SELECT g, 'v' || g FROM generate_series(1, 5000) g;
SELECT (dolt_commit('-Am', 'rst b'))[1] AS rst_b \gset
CREATE FUNCTION pg_temp.since_of(wanted text, known text[]) RETURNS bigint LANGUAGE sql
  AS $$ SELECT count(*) FROM dolt_serve_since(ARRAY[decode(wanted, 'hex')],
		 ARRAY(SELECT decode(k, 'hex') FROM unnest(known) k)) $$;
-- a known commit older than its parent: the parent isn't sent, nor what's
-- beneath it
SELECT test_commit_chain(:'rst_b', 3) AS rst_k \gset
UPDATE rst.t SET v = 'w' WHERE id = 1;
SELECT (dolt_commit('-Am', 'rst w'))[1] AS rst_w \gset
SELECT pg_temp.since_of(:'rst_w', ARRAY[:'rst_k']) = pg_temp.since_of(:'rst_w', ARRAY[:'rst_b']) AS older_known,
	   pg_temp.since_of(:'rst_k', ARRAY[:'rst_b']) AS older_wanted,
	   pg_temp.since_of(:'rst_w', ARRAY[:'rst_w', :'rst_k']) AS none;
-- commits of one time: the known side's longer, its fork point not sent
BEGIN;
UPDATE rst.t SET v = 'p' WHERE id = 2;
SELECT (dolt_commit('-Am', 'rst p'))[1] AS rst_p \gset
SELECT dolt_branch('rst_f');
UPDATE rst.t SET v = 'k1' WHERE id = 3;
SELECT length(dolt_commit('-Am', 'rst k1')::text) AS committed;
UPDATE rst.t SET v = 'k2' WHERE id = 4;
SELECT length(dolt_commit('-Am', 'rst k2')::text) AS committed;
UPDATE rst.t SET v = 'k3' WHERE id = 5;
SELECT (dolt_commit('-Am', 'rst k3'))[1] AS rst_k3 \gset
SELECT dolt_checkout('rst_f') IS NOT NULL AS on_f;
UPDATE rst.t SET v = 'x' WHERE id = 6;
SELECT (dolt_commit('-Am', 'rst x'))[1] AS rst_x \gset
COMMIT;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT pg_temp.since_of(:'rst_x', ARRAY[:'rst_k3']) = pg_temp.since_of(:'rst_x', ARRAY[:'rst_p']) AS tied_known,
	   pg_temp.since_of(:'rst_x', ARRAY[:'rst_p']) > 0 AS some;
-- a wanted commit the store hasn't: nothing of it, nor of the walk
SELECT pg_temp.since_of(repeat('ab', 32), ARRAY[:'rst_b']) AS unknown_wanted,
	   (SELECT count(*) FROM dolt_serve_since(ARRAY[decode(repeat('ab', 32), 'hex'), decode(:'rst_w', 'hex')],
											  ARRAY[decode(:'rst_b', 'hex')]))
	   = pg_temp.since_of(:'rst_w', ARRAY[:'rst_b']) AS with_unknown_wanted;
SELECT dolt_branch('-D', 'rst_f');
DROP SCHEMA rst CASCADE;
SELECT length(dolt_commit('-Am', 'rst gone')::text) AS committed;
