# A copy of a branch's catalog rows waits for DDL on the branch begun
# before it, and copies what that DDL made once it commits: nothing made on
# the branch is left out of, or half in, the copy.

setup
{
	CREATE TABLE branch_ids (n int PRIMARY KEY, b int);
	INSERT INTO branch_ids VALUES (1, pg_branch_id_allocate()), (2, pg_branch_id_allocate());
}

setup
{
	DO $$
	BEGIN
		PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = 1), false);
		CREATE SCHEMA app VERSIONED;
		CREATE TABLE app.t (id int PRIMARY KEY);
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

session ddl
setup		{ DO $$BEGIN PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = 1), false); END$$; }
step d_begin	{ BEGIN; }
step d_make	{ CREATE TABLE app.t2 (id int PRIMARY KEY, t int REFERENCES app.t); }
step d_commit	{ COMMIT; }

session copier
step c_copy	{ SELECT FROM pg_branch_catalog_copy((SELECT b FROM branch_ids WHERE n = 1), (SELECT b FROM branch_ids WHERE n = 2)); }
step c_check	{ DO $$BEGIN PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = 2), false); END$$; SELECT to_regclass('app.t2') IS NOT NULL AS t2_copied, (SELECT count(*) FROM pg_constraint WHERE conrelid = to_regclass('app.t2') AND contype = 'f') AS its_foreign_key; }

permutation d_begin d_make c_copy d_commit c_check
