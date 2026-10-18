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
-- another format: refused
SELECT dolt_serve_move_head('main', pg_temp.h('main'), pg_temp.h('main'), '0', false);

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
