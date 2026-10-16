-- A branch made at a commit has only its head until it is first used:
-- making it copies nothing.  It is listed, resolved, diffed, merged from,
-- branched from, copied and deleted like any other; checking it out (or
-- reading its working set) makes its copies, at its head.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE FUNCTION pg_temp.copied(text) RETURNS bool
  LANGUAGE sql AS $$ SELECT EXISTS (SELECT FROM pg_branch WHERE brname = $1) $$;

CREATE SCHEMA ho VERSIONED;
CREATE TABLE ho.t (id int PRIMARY KEY, v text);
INSERT INTO ho.t VALUES (1, 'one');
SELECT length(dolt_commit('-Am', 'first')::text) AS committed;
UPDATE ho.t SET v = 'uno' WHERE id = 1;	-- uncommitted on main

SELECT dolt_branch('h1');
SELECT pg_temp.copied('h1') AS copied;
SELECT name, hash = dolt_hashof('main') AS at_main_head FROM dolt_branches ORDER BY name;
SELECT dolt_branch('h1');
SELECT dolt_hashof('h1') = dolt_hashof('HEAD') AS resolves;
SELECT diff_type, (from_row).v, (to_row).v FROM dolt_diff('h1', 'WORKING', NULL::ho.t);

-- branched from, copied: still only heads
SELECT dolt_branch('h2', 'h1'), dolt_branch('-c', 'h1', 'h3');
SELECT pg_temp.copied('h2') OR pg_temp.copied('h3') AS copied;

-- checked out: copies made, at the head
SELECT dolt_checkout('h1') IS NOT NULL AS on_h1;
SELECT pg_temp.copied('h1') AS copied;
SELECT * FROM ho.t;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;
INSERT INTO ho.t VALUES (2, 'two');
SELECT length(dolt_commit('-am', 'on h1')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT * FROM ho.t ORDER BY id;

-- one not merged needs -D to delete, copies or not: h5 (only a head) is
-- at h1's commit, which main hasn't
SELECT dolt_branch('h5', 'h1');
SELECT pg_temp.copied('h5') AS copied;
SELECT dolt_branch('-d', 'h5');
SELECT dolt_branch('-d', 'h1');
SELECT dolt_branch('-D', 'h5');

-- merged from: h2, only a head, is at main's head (a no-op); h1 is ahead
SELECT (dolt_merge('h2'))[2:4];
SELECT dolt_reset('--hard');
SELECT (dolt_merge('h1'))[3];
SELECT * FROM ho.t ORDER BY id;

-- deleted once merged: with copies (h1), or only a head (h3, and h4 at
-- an older commit)
SELECT dolt_branch('-d', 'h1', 'h3');
SELECT dolt_branch('h4', dolt_hashof('HEAD~1'));
SELECT dolt_branch('-d', 'h4', 'h2');
SELECT name FROM dolt_branches ORDER BY name;
SELECT dolt_branch('-d', 'nope');

DROP SCHEMA ho CASCADE;
