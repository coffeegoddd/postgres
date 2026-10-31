# An extension is shared or each branch's, never both, and a branch has
# one copy at most: of two sessions making one at once, the second waits
# for the first, and then is refused if the first's committed copy is the
# other kind, or its branch's.  Sessions on two branches each get their
# own.

setup
{
	CREATE TABLE branch_ids (n int PRIMARY KEY, b int);
	INSERT INTO branch_ids VALUES (1, pg_branch_id_allocate()), (2, pg_branch_id_allocate());
	CREATE SCHEMA pub;
}

setup
{
	DO $$
	BEGIN
		FOR i IN 1 .. 2 LOOP
			PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = i), false);
			CREATE SCHEMA app VERSIONED;
			IF i = 1 THEN
				CREATE SCHEMA app2 VERSIONED;
			END IF;
		END LOOP;
	END $$;
}

teardown
{
	DO $$
	BEGIN
		FOR i IN 1 .. 2 LOOP
			PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = i), false);
			DROP SCHEMA app CASCADE;
			DROP SCHEMA IF EXISTS app2 CASCADE;
		END LOOP;
		PERFORM set_config('catalog_branch', '0', false);
		DROP SCHEMA pub CASCADE;
		DROP TABLE branch_ids;
	END $$;
}

session a
setup		{ SET client_min_messages = warning; RESET catalog_branch; }
step a_b1	{ DO $$BEGIN PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = 1), false); END$$; }
step a_begin	{ BEGIN; }
step a_app	{ CREATE EXTENSION pg_trgm SCHEMA app; }
step a_pub	{ CREATE EXTENSION pg_trgm SCHEMA pub; }
step a_commit	{ COMMIT; }

session b
setup		{ SET client_min_messages = warning; RESET catalog_branch; }
step b_b1	{ DO $$BEGIN PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = 1), false); END$$; }
step b_b2	{ DO $$BEGIN PERFORM set_config('catalog_branch', (SELECT b::text FROM branch_ids WHERE n = 2), false); END$$; }
step b_app	{ CREATE EXTENSION pg_trgm SCHEMA app; }
step b_app2	{ CREATE EXTENSION pg_trgm SCHEMA app2; }
step b_pub	{ CREATE EXTENSION pg_trgm SCHEMA pub; }
step b_count	{ SELECT count(*) FROM pg_extension WHERE extname = 'pg_trgm'; }

# a branch's copy, then a shared one: refused
permutation a_b1 a_begin a_app b_pub a_commit
# a shared copy, then a branch's: refused
permutation a_begin a_pub b_b1 b_app a_commit
# two copies on one branch, in two of its schemas: refused
permutation a_b1 a_begin a_app b_b1 b_app2 a_commit
# a copy on each of two branches: both made
permutation a_b1 a_begin a_app b_b2 b_app a_commit b_count
