# Dropping a branch's catalog rows waits for a session making a versioned
# schema on the branch, and drops it too once it commits: no schema of
# the branch outlives the drop.

setup
{
	CREATE TABLE branch_ids (n int PRIMARY KEY, b int);
	INSERT INTO branch_ids VALUES (1, pg_branch_id_allocate());
}

teardown
{
	DO $$
	BEGIN
		PERFORM pg_branch_catalog_drop((SELECT b FROM branch_ids WHERE n = 1));
		DROP TABLE branch_ids;
	END $$;
}

session maker
setup		{ DO $$BEGIN PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = 1), false); END$$; }
step m_begin	{ BEGIN; }
step m_make	{ CREATE SCHEMA app VERSIONED CREATE TABLE t (x int); }
step m_commit	{ COMMIT; }

session dropper
step d_drop	{ SELECT FROM pg_branch_catalog_drop((SELECT b FROM branch_ids WHERE n = 1)); }
step d_count	{ SET catalog_branch_filter = off; SELECT count(*) AS schemas FROM pg_namespace WHERE nspbranch = (SELECT b FROM branch_ids WHERE n = 1); RESET catalog_branch_filter; }

permutation m_begin m_make d_drop m_commit d_count
