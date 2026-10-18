-- Doltgres's scripts on foreign keys across schemas and merges that leave
-- violations (testing/go/foreign_keys_test.go at doltgresql d5e05f46), and
-- on who may call the version-control functions (auth_test.go, "Basic user
-- authorization for SELECT executing Dolt stored procedures"), run on
-- prolly.  A comment says where prolly's answer differs, and why.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE FUNCTION pg_temp.committed(VARIADIC args text[]) RETURNS bool LANGUAGE sql AS $$
	SELECT length((dolt_commit(VARIADIC args))[1]) = 64
$$;

-- "foreign key in another schema with search path": the parent found by
-- the search path, a table of its name in a schema not on it ignored
CREATE SCHEMA dgk_parent VERSIONED;
CREATE SCHEMA dgk_child VERSIONED;
CREATE SCHEMA dgk_fake VERSIONED;
SELECT pg_temp.committed('-Am', 'create schemas');
SET search_path TO dgk_parent, dgk_child;
CREATE TABLE dgk_parent.parent (pk int, val int, primary key(pk));
CREATE TABLE dgk_fake.parent (pk int, val int, primary key(pk));
CREATE TABLE dgk_child.child (id int, info varchar(255), test_pk int, primary key(id), foreign key (test_pk) references parent(pk));
INSERT INTO parent VALUES (0, 0), (1, 1), (2, 2);
SELECT dolt_add('.');
SELECT * FROM dolt_status WHERE table_name LIKE 'dgk_%' ORDER BY 1;
SELECT pg_temp.committed('-am', 'new tables');
SELECT * FROM dolt_status WHERE table_name LIKE 'dgk_%';
INSERT INTO child VALUES (2, 'two', 2);
INSERT INTO child VALUES (3, 'three', 3);
SELECT * FROM dgk_child.child;
RESET search_path;
SELECT pg_temp.committed('-am', 'child row');

-- "merge with constraint violations": refused outside a transaction
-- block; kept with dolt.allow_commit_conflicts (Doltgres's
-- dolt_force_transaction_commit), the violation listed as Dolt's
-- (prolly's violation_info lists the foreign key's name, tables and
-- columns; Dolt's also its indexes and actions)
CREATE SCHEMA dgk_merge VERSIONED;
SET search_path = dgk_merge;
CREATE TABLE parent (a INT PRIMARY KEY, b INT UNIQUE);
CREATE TABLE child (c INT PRIMARY KEY, d INT);
ALTER TABLE child ADD CONSTRAINT fk FOREIGN KEY (d) REFERENCES parent(b);
INSERT INTO parent VALUES (1, 1), (2, 2), (3, 3);
INSERT INTO child VALUES (1, 1), (2, 2);
SELECT pg_temp.committed('-Am', 'initial commit');
SELECT dolt_branch('dgk_feature');
INSERT INTO child VALUES (3, 3);
SELECT pg_temp.committed('-Am', 'new child');
SELECT dolt_checkout('dgk_feature');
DELETE FROM parent WHERE b = 3;
SELECT pg_temp.committed('-Am', 'delete from parent');
SELECT dolt_merge('main');
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('main'))[2:4];
SELECT * FROM dolt_constraint_violations() WHERE table_name LIKE 'dgk_merge.%';
SELECT violation_type, violating_row->>'c' AS c, violating_row->>'d' AS d, violation_info
  FROM dolt_constraint_violations('dgk_merge.child');
SELECT dolt_reset('--hard');
RESET dolt.allow_commit_conflicts;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dgk_feature');
RESET search_path;

-- "Merge foreign keys across types, varchar -> text, violation"
CREATE SCHEMA dgk_types VERSIONED;
SET search_path = dgk_types;
CREATE TABLE parent (a TEXT PRIMARY KEY);
CREATE TABLE child (b INT PRIMARY KEY, c varchar(255), CONSTRAINT fk FOREIGN KEY (c) REFERENCES parent(a) ON DELETE CASCADE ON UPDATE NO ACTION);
INSERT INTO parent (a) VALUES ('abc'), ('def');
INSERT INTO child (b, c) VALUES (1, 'abc'), (2, 'def');
SELECT pg_temp.committed('-Am', '1');
SELECT dolt_branch('dgk_other_branch');
DELETE FROM child WHERE b = 1;
DELETE FROM parent WHERE a = 'abc';
SELECT pg_temp.committed('-Am', '2');
SELECT dolt_checkout('dgk_other_branch');
INSERT INTO child (b, c) VALUES (3, 'abc');
SELECT pg_temp.committed('-Am', '3');
SET dolt.allow_commit_conflicts = on;
SELECT strpos(dolt_merge('main')::text, 'merge successful') > 0 AS merge_successful;
SELECT violation_type, violating_row->>'b' AS b, violating_row->>'c' AS c
  FROM dolt_constraint_violations('dgk_types.child');
SELECT dolt_reset('--hard');
RESET dolt.allow_commit_conflicts;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dgk_other_branch');
RESET search_path;

-- auth_test.go, "Basic user authorization for SELECT executing Dolt stored
-- procedures": a role with privileges on a schema's tables (and, here,
-- CREATE on the database: a branch is the database's, as Postgres asks
-- for making a schema) branches, checks out, commits, picks, merges and
-- resolves, though it can't use another versioned schema here; it may not
-- collect garbage (the database's owner or pg_maintain, as VACUUM asks).
-- Unlike Doltgres's, it may add a remote, which is the database's own
-- setting, as a branch is.  test_table has no primary key: its rows are
-- keyed by content and never conflict (Doltgres reports the two updates
-- as a conflict, resolved with --theirs), so the merge keeps both.
CREATE SCHEMA dgk_auth VERSIONED;
CREATE TABLE dgk_auth.test_table (v int);
INSERT INTO dgk_auth.test_table VALUES (1);
SELECT pg_temp.committed('-Am', 'add test table');
CREATE ROLE regress_dgk_basic;
SELECT current_database() AS db \gset
-- (errors naming the database, by another name: it differs between runs)
CREATE FUNCTION pg_temp.fails(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE q;
  RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
  RETURN replace(SQLERRM, current_database(), 'DB');
END $$;
GRANT USAGE, CREATE ON SCHEMA dgk_auth TO regress_dgk_basic;
GRANT SELECT, INSERT, DELETE, UPDATE ON dgk_auth.test_table TO regress_dgk_basic;
SET ROLE regress_dgk_basic;
SET search_path = dgk_auth;
SELECT pg_temp.fails($$SELECT dolt_checkout('-b', 'dgk_test')$$);
RESET ROLE;
GRANT CREATE ON DATABASE :db TO regress_dgk_basic;
SET ROLE regress_dgk_basic;
SELECT dolt_checkout('-b', 'dgk_test');
SELECT dolt_branch('dgk_new_branch');
INSERT INTO test_table VALUES (2);
SELECT dolt_add('.');
SELECT pg_temp.committed('-m', 'amend test table');
SELECT dolt_checkout('main');
SELECT (dolt_cherry_pick('dgk_test'))[2:4];
SELECT dolt_checkout('-b', 'dgk_conflict');
UPDATE test_table SET v = -1 WHERE v = 1;
SELECT pg_temp.committed('-am', 'amend 1 to -1');
SELECT dolt_checkout('main');
UPDATE test_table SET v = -2 WHERE v = 1;
SELECT pg_temp.committed('-am', 'amend 2 to -2');
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('dgk_conflict'))[2:4];
SELECT dolt_conflicts_resolve('--theirs', 'test_table');
SELECT pg_temp.committed('-am', 'resolve conflicts');
RESET dolt.allow_commit_conflicts;
SELECT * FROM test_table ORDER BY v;
SELECT pg_temp.fails($$SELECT dolt_gc()$$);
SELECT dolt_remote('add', 'dgk_origin', 'file:///nonexistent');
SELECT dolt_verify_constraints('--all');
-- A merge that changes a definition replays it as its DDL would run, as
-- the role: a check another branch added, on a table the role may write
-- but in a versioned schema it can't use, needs the schema's USAGE (and,
-- as ALTER TABLE does, the table's ownership); refused, the merge leaves
-- nothing behind.
RESET ROLE;
CREATE SCHEMA dgk_hidden VERSIONED;
CREATE TABLE dgk_hidden.h (id int PRIMARY KEY, v int);
INSERT INTO dgk_hidden.h VALUES (1, 5), (2, -5);
GRANT SELECT, INSERT, UPDATE, DELETE ON dgk_hidden.h TO regress_dgk_basic;
SELECT pg_temp.committed('-Am', 'hidden table');
SELECT dolt_checkout('-b', 'dgk_hidden_check');
DELETE FROM dgk_hidden.h WHERE id = 2;
ALTER TABLE dgk_hidden.h ADD CONSTRAINT h_positive CHECK (v > 0);
SELECT pg_temp.committed('-Am', 'positive');
SELECT dolt_checkout('main');
SET ROLE regress_dgk_basic;
SELECT (dolt_merge('dgk_hidden_check'))[3:4];
RESET ROLE;
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';
SELECT dolt_reset('--hard');
SET ROLE regress_dgk_basic;
SELECT dolt_checkout('dgk_test');
SELECT dolt_checkout('main');
RESET search_path;
RESET ROLE;
SELECT dolt_branch('-D', 'dgk_test', 'dgk_new_branch', 'dgk_conflict', 'dgk_hidden_check');
SELECT dolt_remote('remove', 'dgk_origin');
REVOKE CREATE ON DATABASE :db FROM regress_dgk_basic;

SET client_min_messages = warning;
DROP SCHEMA dgk_parent, dgk_child, dgk_fake, dgk_merge, dgk_types, dgk_auth, dgk_hidden CASCADE;
DROP OWNED BY regress_dgk_basic;
DROP ROLE regress_dgk_basic;
RESET client_min_messages;
SELECT pg_temp.committed('-Am', 'doltgres foreign keys and auth done');
