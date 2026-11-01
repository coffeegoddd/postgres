# Each branch's table is a relation of its own: row locks, serializable
# predicate locks and DDL on one branch's table never wait for, or fail
# because of, a session on another branch's table of the same name.

setup
{
	CREATE TABLE branch_ids (n int PRIMARY KEY, b int);
	INSERT INTO branch_ids VALUES (1, pg_branch_id_allocate()), (2, pg_branch_id_allocate());
}

setup
{
	DO $$
	BEGIN
		FOR i IN 1 .. 2 LOOP
			PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = i), false);
			CREATE SCHEMA app VERSIONED;
			CREATE TABLE app.t (id int PRIMARY KEY, v int);
			INSERT INTO app.t VALUES (1, 10), (2, 20);
		END LOOP;
		PERFORM set_config('catalog_branch', '0', false);
	END $$;
}

teardown
{
	DO $$
	BEGIN
		PERFORM pg_branch_catalog_drop(b) FROM branch_ids;
		DROP TABLE branch_ids;
	END $$;
}

session one
setup		{ DO $$BEGIN PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = 1), false); END$$; }
step o_begin	{ BEGIN; }
step o_serial	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step o_lock	{ SELECT v FROM app.t WHERE id = 1 FOR UPDATE; }
step o_read	{ SELECT sum(v) FROM app.t; }
step o_write	{ UPDATE app.t SET v = v + 1 WHERE id = 1; }
step o_ddl	{ ALTER TABLE app.t ADD COLUMN extra int; }
step o_make	{ CREATE TABLE app.only (x int); }
step o_commit	{ COMMIT; }

session two
setup		{ DO $$BEGIN PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = 2), false); END$$; }
step w_begin	{ BEGIN; }
step w_serial	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step w_update	{ UPDATE app.t SET v = v + 1 WHERE id = 1; }
step w_read	{ SELECT sum(v) FROM app.t; }
step w_write	{ UPDATE app.t SET v = v + 1 WHERE id = 2; }
step w_make	{ CREATE TABLE app.only (y int, z int); }
step w_commit	{ COMMIT; }

# a row lock on one branch's row 1 doesn't hold up an update of the other's
permutation o_begin o_lock w_update o_commit
# serializable transactions reading and writing the "same" table on two
# branches: no serialization failure, they share no data
permutation o_serial w_serial o_read w_read o_write w_write o_commit w_commit
# DDL on one branch's table while a session reads the other's: no wait
permutation w_begin w_read o_ddl w_commit
# the same name made on both branches at once, both transactions open
permutation o_begin w_begin o_make w_make o_commit w_commit
