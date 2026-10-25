-- A table a merge (or a fast-forward) makes on the branch, new to it, has
-- the privileges the source branch's table of its name has, with the ones its
-- making gives it (the branch's default privileges): commits hold
-- no privileges, but a branch of this database does.  One a remote's ref
-- brings (a fetch, a pull) arrives with only the ones its making gives it.
-- A cherry-pick's or a revert's, of a commit a local branch has, the same.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
DROP ROLE IF EXISTS regress_mg_r;
DROP ROLE IF EXISTS regress_mg_w;
DROP ROLE IF EXISTS regress_mg_alice;
DROP ROLE IF EXISTS regress_mg_bob;
RESET client_min_messages;
CREATE ROLE regress_mg_r;
CREATE ROLE regress_mg_w;
-- (a relation's grants but its owner's, its columns' too)
CREATE FUNCTION pg_temp.grants(r regclass) RETURNS text LANGUAGE sql AS $$
SELECT coalesce(string_agg(g, ', ' ORDER BY g), 'none') FROM (
	SELECT a.grantee::regrole || ' ' || a.privilege_type AS g
	FROM pg_class c, aclexplode(c.relacl) a
	WHERE c.oid = r AND a.grantee <> c.relowner
	UNION ALL
	SELECT a.grantee::regrole || ' ' || a.privilege_type || ' (' || t.attname || ')'
	FROM pg_attribute t, aclexplode(t.attacl) a
	WHERE t.attrelid = r AND t.attnum > 0) s
$$;

CREATE SCHEMA mg VERSIONED;
CREATE TABLE mg.t (id int PRIMARY KEY);
GRANT USAGE ON SCHEMA mg TO regress_mg_r, regress_mg_w;
GRANT SELECT ON mg.t TO regress_mg_w;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;

-- Made and granted on a branch, then fast-forwarded into main.
SELECT dolt_checkout('-b', 'mg_ff') IS NOT NULL AS on_ff;
CREATE TABLE mg.n (id serial PRIMARY KEY, a int, b int);
GRANT SELECT ON mg.n TO regress_mg_r;
GRANT INSERT (a) ON mg.n TO regress_mg_w;
GRANT USAGE ON SEQUENCE mg.n_id_seq TO regress_mg_w;
INSERT INTO mg.n (a, b) VALUES (1, 2);
SELECT length(dolt_commit('-Am', 'n')::text) AS committed;
SELECT 'mg.n'::regclass::oid AS ff_oid \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('mg_ff'))[2:2] AS fast_forward;
SELECT 'mg.n'::regclass::oid <> :ff_oid AS new_relation,
	   pg_temp.grants('mg.n'), pg_temp.grants('mg.n_id_seq') AS seq_grants;
-- (the grantee reads it, and writes the column it was given)
SET ROLE regress_mg_r;
SELECT a, b FROM mg.n;
RESET ROLE;
SET ROLE regress_mg_w;
INSERT INTO mg.n (a) VALUES (3);
INSERT INTO mg.n (a, b) VALUES (4, 4);
RESET ROLE;
-- (a table on both branches keeps main's privileges)
SELECT pg_temp.grants('mg.t');
-- (shared dependencies recorded on main: the role can't be dropped)
DROP ROLE regress_mg_r;
SELECT dolt_reset('--hard') IS NOT NULL AS reset;

-- Made on a branch while main moved on: merged with a commit, the same.
SELECT dolt_checkout('-b', 'mg_m') IS NOT NULL AS on_m;
CREATE TABLE mg.m (id int PRIMARY KEY, c text);
GRANT SELECT, UPDATE (c) ON mg.m TO regress_mg_r;
-- (its row security, which commits hold, comes too)
ALTER TABLE mg.m ENABLE ROW LEVEL SECURITY;
CREATE POLICY mg_p ON mg.m FOR SELECT TO regress_mg_r USING (id > 1);
INSERT INTO mg.m VALUES (1, 'one'), (2, 'two');
SELECT length(dolt_commit('-Am', 'm')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
-- (main's default privileges, which the branch hasn't, apply as well)
ALTER DEFAULT PRIVILEGES IN SCHEMA mg GRANT UPDATE ON TABLES TO regress_mg_w;
CREATE TABLE mg.o (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'o')::text) AS committed;
SELECT (dolt_merge('mg_m', '-m', 'merged m'))[2:2] AS fast_forward;
SELECT pg_temp.grants('mg.m');
SET ROLE regress_mg_r;
SELECT id, c FROM mg.m;
RESET ROLE;
-- (main's own new table: main's default privileges only, as main made it)
SELECT pg_temp.grants('mg.o');
ALTER DEFAULT PRIVILEGES IN SCHEMA mg REVOKE UPDATE ON TABLES FROM regress_mg_w;

-- --no-ff where a fast-forward would do.
SELECT dolt_checkout('-b', 'mg_noff') IS NOT NULL AS on_noff;
CREATE TABLE mg.q (id int PRIMARY KEY);
GRANT SELECT ON mg.q TO regress_mg_r;
SELECT length(dolt_commit('-Am', 'q')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('--no-ff', 'mg_noff', '-m', 'merged q'))[2:2] AS fast_forward;
SELECT pg_temp.grants('mg.q');

-- The other way: made and granted on main, merged into a branch.
SELECT dolt_checkout('-b', 'mg_rev') IS NOT NULL AS on_rev;
CREATE TABLE mg.rev (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'rev')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
CREATE TABLE mg.back (id int PRIMARY KEY);
GRANT SELECT ON mg.back TO regress_mg_r;
SELECT length(dolt_commit('-Am', 'back')::text) AS committed;
SELECT dolt_checkout('mg_rev') IS NOT NULL AS on_rev;
SELECT (dolt_merge('main', '-m', 'main in'))[2:2] AS fast_forward;
SELECT pg_temp.grants('mg.back');
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- Integrated: dolt_integrate() merges the branch into main, the same.
SELECT dolt_checkout('-b', 'mg_int') IS NOT NULL AS on_int;
CREATE TABLE mg.i (id int PRIMARY KEY);
GRANT SELECT ON mg.i TO regress_mg_r;
SELECT (dolt_integrate('main', '-m', 'i in'))[2] AS fast_forward;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT pg_temp.grants('mg.i');

-- Cherry-picked: the commit comes from a local branch, the same.
SELECT dolt_checkout('-b', 'mg_cp') IS NOT NULL AS on_cp;
CREATE TABLE mg.cp (id int PRIMARY KEY);
GRANT SELECT ON mg.cp TO regress_mg_r;
SELECT length(dolt_commit('-Am', 'cp')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT length(dolt_cherry_pick('mg_cp')::text) > 0 AS picked;
SELECT pg_temp.grants('mg.cp');
-- (by its hash, too: the branch whose history has it)
SELECT dolt_checkout('-b', 'mg_cp2') IS NOT NULL AS on_cp2;
CREATE TABLE mg.cp2 (id int PRIMARY KEY);
GRANT SELECT ON mg.cp2 TO regress_mg_w;
SELECT length(dolt_commit('-Am', 'cp2')::text) AS committed;
SELECT dolt_hashof('HEAD') AS cp2_commit \gset
SELECT length(dolt_commit('--allow-empty', '-m', 'past cp2')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT length(dolt_cherry_pick(:'cp2_commit')::text) > 0 AS picked;
SELECT pg_temp.grants('mg.cp2');

-- Reverted: a revert of a drop makes the table again, with the grants of a
-- local branch whose history has the dropping commit and the table.
SELECT dolt_checkout('-b', 'mg_rv') IS NOT NULL AS on_rv;
CREATE TABLE mg.rv (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'rv')::text) AS committed;
DROP TABLE mg.rv;
SELECT length(dolt_commit('-Am', 'rv gone')::text) AS committed;
SELECT dolt_hashof('HEAD') AS rv_drop \gset
SELECT dolt_checkout('-b', 'mg_rv2') IS NOT NULL AS on_rv2;
CREATE TABLE mg.rv (id int PRIMARY KEY);
GRANT SELECT ON mg.rv TO regress_mg_r;
SELECT length(dolt_commit('-Am', 'rv again')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('mg_rv'))[2:2] AS fast_forward;
SELECT length(dolt_revert(:'rv_drop')::text) > 0 AS reverted;
SELECT pg_temp.grants('mg.rv');

-- Fetched: pushed to a remote and fetched back, the branch deleted here,
-- its table arrives with none.
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/mg-' ||
	txid_current() AS mg_url \gset
SELECT dolt_checkout('-b', 'mg_far') IS NOT NULL AS on_far;
CREATE TABLE mg.far (id int PRIMARY KEY);
GRANT SELECT ON mg.far TO regress_mg_r;
SELECT length(dolt_commit('-Am', 'far')::text) AS committed;
SELECT dolt_remote('add', 'mg_origin', :'mg_url');
SELECT dolt_push('mg_origin', 'mg_far') AS pushed;
SELECT dolt_checkout('-b', 'mg_far2') IS NOT NULL AS on_far2;
CREATE TABLE mg.far2 (id int PRIMARY KEY);
GRANT SELECT ON mg.far2 TO regress_mg_r;
SELECT length(dolt_commit('-Am', 'far2')::text) AS committed;
SELECT dolt_push('mg_origin', 'mg_far2') AS pushed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'mg_far');
SELECT dolt_branch('-D', 'mg_far2');
SELECT dolt_fetch('mg_origin') AS fetched;
SELECT (dolt_merge('mg_origin/mg_far'))[2:2] AS fast_forward;
SELECT pg_temp.grants('mg.far');
-- (and a fetched commit, cherry-picked, the same)
SELECT length(dolt_cherry_pick('mg_origin/mg_far2')::text) > 0 AS picked;
SELECT pg_temp.grants('mg.far2');
SELECT dolt_remote('remove', 'mg_origin');

-- The source's relation must be the one the merged commit's was made
-- from: one of the name made since, uncommitted, gives nothing.  Here a
-- sequence in a committed table's place (its USAGE no table's privilege):
SELECT dolt_checkout('-b', 'mg_k') IS NOT NULL AS on_k;
CREATE TABLE mg.k (id int PRIMARY KEY, secret text);
CREATE TABLE mg.k2 (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'k')::text) AS committed;
DROP TABLE mg.k;
CREATE SEQUENCE mg.k;
GRANT USAGE, SELECT, UPDATE ON SEQUENCE mg.k TO regress_mg_r;
-- and a private committed table made again, granted to everyone:
DROP TABLE mg.k2;
CREATE TABLE mg.k2 (id int PRIMARY KEY);
GRANT SELECT ON mg.k2 TO PUBLIC;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('mg_k'))[2:2] AS fast_forward;
SELECT relkind, pg_temp.grants('mg.k') FROM pg_class WHERE oid = 'mg.k'::regclass;
SELECT pg_temp.grants('mg.k2');

-- The table's maker keeps its own privileges, a grant to it from the
-- merging role, which owns the table here.
CREATE ROLE regress_mg_alice;
CREATE ROLE regress_mg_bob;
SELECT current_database() AS mg_db \gset
GRANT CREATE ON DATABASE :"mg_db" TO regress_mg_alice, regress_mg_bob;
GRANT USAGE, CREATE ON SCHEMA mg TO regress_mg_alice, regress_mg_bob;
SET ROLE regress_mg_alice;
SELECT dolt_checkout('-b', 'mg_al') IS NOT NULL AS on_al;
CREATE TABLE mg.al (id int PRIMARY KEY);
GRANT SELECT ON mg.al TO regress_mg_r;
SELECT length(dolt_commit('-Am', 'al')::text) AS committed;
RESET ROLE;
SET ROLE regress_mg_bob;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('mg_al'))[2:2] AS fast_forward;
RESET ROLE;
SELECT c.relowner::regrole,
	   (SELECT string_agg(a.privilege_type, ',' ORDER BY a.privilege_type)
		FROM aclexplode(c.relacl) a
		WHERE a.grantee = 'regress_mg_alice'::regrole) AS maker_privileges,
	   has_table_privilege('regress_mg_r', c.oid, 'SELECT') AS grantee_reads
FROM pg_class c WHERE c.oid = 'mg.al'::regclass;
SET ROLE regress_mg_alice;
SELECT count(*) FROM mg.al;
RESET ROLE;

-- A branch named as another's full ref name is the one merged, and the
-- one whose privileges come.
SELECT dolt_checkout('-b', 'mg_x') IS NOT NULL AS on_x;
CREATE TABLE mg.x (id int PRIMARY KEY);
GRANT SELECT ON mg.x TO regress_mg_r;
SELECT length(dolt_commit('-Am', 'x')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('refs/heads/mg_x') AS named;
SELECT dolt_checkout('refs/heads/mg_x') IS NOT NULL AS on_full;
CREATE TABLE mg.x (id int PRIMARY KEY);
GRANT SELECT ON mg.x TO regress_mg_w;
SELECT length(dolt_commit('-Am', 'x too')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('refs/heads/mg_x'))[2:2] AS fast_forward;
SELECT pg_temp.grants('mg.x');

SELECT dolt_branch('-D', 'mg_ff');
SELECT dolt_branch('-D', 'mg_m');
SELECT dolt_branch('-D', 'mg_noff');
SELECT dolt_branch('-D', 'mg_rev');
SELECT dolt_branch('-D', 'mg_int');
SELECT dolt_branch('-D', 'mg_cp');
SELECT dolt_branch('-D', 'mg_cp2');
SELECT dolt_branch('-D', 'mg_rv');
SELECT dolt_branch('-D', 'mg_rv2');
SELECT dolt_branch('-D', 'mg_k');
SELECT dolt_branch('-D', 'mg_al');
SELECT dolt_branch('-D', 'mg_x');
SELECT dolt_branch('-D', 'refs/heads/mg_x');
DROP SCHEMA mg CASCADE;
SELECT length(dolt_commit('-Am', 'mg gone')::text) AS committed;
DROP ROLE regress_mg_r;
DROP ROLE regress_mg_w;
REVOKE CREATE ON DATABASE :"mg_db" FROM regress_mg_alice, regress_mg_bob;
DROP ROLE regress_mg_alice;
DROP ROLE regress_mg_bob;
