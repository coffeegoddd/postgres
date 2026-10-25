-- A table or materialized view a move makes from a commit (a branch's
-- first use, a reset, a rebase's end, a merge, a pick, a revert, a stash's
-- apply) is made by the owner the commit's record of relations names, with
-- no grants: its statements, its rows, the checks of them and its
-- materialized view's query run as that owner, restricted
-- (SECURITY_RESTRICTED_OPERATION), as REFRESH runs them.  The role moving
-- runs none of the owner's code, gains no relation, and reads nothing it
-- wasn't granted, through the table or a materialized view over it.  A merge
-- adds the source branch's grants on top.
CREATE DATABASE regress_recorded_makers;
\c regress_recorded_makers
CREATE EXTENSION test_prolly;
CREATE SCHEMA rm VERSIONED;
CREATE ROLE regress_rm_alice IN ROLE pg_prolly_write;
CREATE ROLE regress_rm_w IN ROLE pg_prolly_write;
CREATE ROLE regress_rm_spy;
GRANT USAGE, CREATE ON SCHEMA rm TO regress_rm_alice, regress_rm_w;
GRANT USAGE ON SCHEMA rm TO regress_rm_spy;
SELECT format('GRANT CREATE ON DATABASE %I TO regress_rm_alice, regress_rm_w',
			  current_database()) \gexec
-- (who owns a relation, and whether the writer may read it)
CREATE FUNCTION pg_temp.made(r text) RETURNS TABLE (owner regrole, writer_reads bool, grants aclitem[])
LANGUAGE sql AS $$
	SELECT c.relowner::regrole,
		has_table_privilege('regress_rm_w', c.oid, 'SELECT'), c.relacl
	FROM pg_class c WHERE c.oid = to_regclass(r)
$$;

SET ROLE regress_rm_alice;
CREATE TABLE rm.t (id int PRIMARY KEY, note text);
INSERT INTO rm.t VALUES (1, 'c1 secret');
CREATE MATERIALIZED VIEW rm.mv AS SELECT * FROM rm.t;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'c1')::text) AS committed;
SELECT dolt_hashof('HEAD') AS c1 \gset
SELECT dolt_branch('x') IS NOT NULL AS made;
SELECT dolt_branch('r') IS NOT NULL AS made;
SET ROLE regress_rm_alice;
DROP TABLE rm.t CASCADE;
CREATE TABLE rm.t (id int PRIMARY KEY, note text);
INSERT INTO rm.t VALUES (2, 'c2 public');
GRANT SELECT ON rm.t TO regress_rm_w;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'c2')::text) AS committed;

-- a branch's first use makes the head's table and materialized view as
-- their recorded owner: the writer reads neither
SET ROLE regress_rm_w;
SELECT dolt_checkout('x') IS NOT NULL AS on_x;
SELECT * FROM pg_temp.made('rm.t');
SELECT * FROM pg_temp.made('rm.mv');
SELECT * FROM rm.mv;
SELECT * FROM rm.t;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
RESET ROLE;

-- a reset to a commit holding them, on a branch where their owner dropped
-- them: the same
SELECT dolt_checkout('r') IS NOT NULL AS on_r;
SET ROLE regress_rm_alice;
DROP TABLE rm.t CASCADE;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'r dropped')::text) AS committed;
SET ROLE regress_rm_w;
SELECT dolt_reset('--hard', :'c1') IS NOT NULL AS reset;
SELECT * FROM pg_temp.made('rm.t');
SELECT * FROM pg_temp.made('rm.mv');
SELECT * FROM rm.mv;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
RESET ROLE;

-- a rebase's end, the branch's copy of the table replaced by the
-- upstream's relation: the upstream's materialized view over it is its
-- owner's too
SELECT dolt_branch('feat') IS NOT NULL AS made;
SELECT dolt_checkout('feat') IS NOT NULL AS on_feat;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_rm_alice;
DROP TABLE rm.t;
CREATE TABLE rm.t (id int PRIMARY KEY, note text);
INSERT INTO rm.t VALUES (3, 'main, made again: secret');
CREATE MATERIALIZED VIEW rm.tmv AS SELECT * FROM rm.t;
GRANT INSERT, UPDATE, DELETE ON rm.t TO regress_rm_w;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'made again on main')::text) AS committed;
SET ROLE regress_rm_w;
SELECT dolt_checkout('feat') IS NOT NULL AS on_feat;
CREATE TABLE rm.f (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'f1')::text) AS committed;
SELECT (dolt_rebase('main'))[1] AS status;
SELECT * FROM pg_temp.made('rm.t');
SELECT * FROM pg_temp.made('rm.tmv');
SELECT * FROM rm.tmv;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
RESET ROLE;

-- the owner's code (a check's function) runs as the owner, restricted,
-- when a superuser's reset makes the table: it can't make its owner a
-- superuser, nor write what only the mover may
CREATE SCHEMA rm_pub;
CREATE TABLE rm_pub.mover_only (who name);
CREATE TABLE rm_pub.log (who name, what text);
GRANT USAGE ON SCHEMA rm_pub TO regress_rm_alice;
GRANT INSERT ON rm_pub.log TO regress_rm_alice;
CREATE FUNCTION rm_pub.f(int) RETURNS int LANGUAGE plpgsql VOLATILE AS $$
BEGIN
	INSERT INTO rm_pub.log VALUES (current_user, 'checked, search_path ' || current_setting('search_path'));
	IF (SELECT rolsuper FROM pg_roles WHERE rolname = current_user) THEN
		EXECUTE 'ALTER ROLE regress_rm_alice SUPERUSER';
	END IF;
	BEGIN
		INSERT INTO rm_pub.mover_only VALUES (current_user);
	EXCEPTION WHEN insufficient_privilege THEN
		INSERT INTO rm_pub.log VALUES (current_user, 'mover_only denied');
	END;
	RETURN $1;
END $$;
ALTER FUNCTION rm_pub.f(int) OWNER TO regress_rm_alice;
SET ROLE regress_rm_alice;
CREATE TABLE rm.e (id int PRIMARY KEY, CONSTRAINT e_chk CHECK (rm_pub.f(id) > 0));
INSERT INTO rm.e VALUES (1);
RESET ROLE;
SELECT length(dolt_commit('-Am', 'e')::text) AS committed;
SELECT dolt_hashof('HEAD') AS e1 \gset
SET ROLE regress_rm_alice;
DROP TABLE rm.e;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'e dropped')::text) AS committed;
TRUNCATE rm_pub.log;
SELECT dolt_reset('--hard', :'e1') IS NOT NULL AS reset;
SELECT rolsuper AS owner_superuser FROM pg_roles WHERE rolname = 'regress_rm_alice';
SELECT DISTINCT who, what FROM rm_pub.log ORDER BY 1, 2;
SELECT count(*) AS mover_only_rows FROM rm_pub.mover_only;
SELECT * FROM pg_temp.made('rm.e');
DROP TABLE rm.e;
SELECT length(dolt_commit('-Am', 'e dropped again')::text) AS committed;

-- the sequences a made table owns (a serial's, an identity's) are its
-- owner's with no grants: neither the mover's default privileges nor its
-- owner's give them any
SET ROLE regress_rm_alice;
CREATE TABLE rm.sq (id serial PRIMARY KEY, n int GENERATED ALWAYS AS IDENTITY, note text);
INSERT INTO rm.sq (note) VALUES ('sq secret');
RESET ROLE;
SELECT length(dolt_commit('-Am', 'sq')::text) AS committed;
SELECT dolt_hashof('HEAD') AS sq1 \gset
SET ROLE regress_rm_alice;
DROP TABLE rm.sq;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'sq dropped')::text) AS committed;
ALTER DEFAULT PRIVILEGES FOR ROLE regress_rm_alice GRANT SELECT ON SEQUENCES TO regress_rm_spy;
SET ROLE regress_rm_w;
ALTER DEFAULT PRIVILEGES GRANT SELECT ON SEQUENCES TO regress_rm_spy;
ALTER DEFAULT PRIVILEGES GRANT SELECT ON TABLES TO regress_rm_spy;
SELECT dolt_reset('--hard', :'sq1') IS NOT NULL AS reset;
RESET ROLE;
SELECT * FROM pg_temp.made('rm.sq');
SELECT * FROM pg_temp.made('rm.sq_id_seq');
SELECT * FROM pg_temp.made('rm.sq_n_seq');
SELECT has_sequence_privilege('regress_rm_spy', 'rm.sq_id_seq', 'SELECT') AS spy_reads_serial,
	has_sequence_privilege('regress_rm_spy', 'rm.sq_n_seq', 'SELECT') AS spy_reads_identity;
SELECT nextval('rm.sq_id_seq') AS next_id;
SET ROLE regress_rm_w;
ALTER DEFAULT PRIVILEGES REVOKE SELECT ON SEQUENCES FROM regress_rm_spy;
ALTER DEFAULT PRIVILEGES REVOKE SELECT ON TABLES FROM regress_rm_spy;
RESET ROLE;
ALTER DEFAULT PRIVILEGES FOR ROLE regress_rm_alice REVOKE SELECT ON SEQUENCES FROM regress_rm_spy;

-- merges, picks, reverts and stash applies make a table as its recorded
-- owner too, never as the mover: a writer can't get a committed table of
-- another role's back as its own
SET ROLE regress_rm_alice;
CREATE TABLE rm.p (id int PRIMARY KEY, note text);
INSERT INTO rm.p VALUES (1, 'p secret');
RESET ROLE;
SELECT length(dolt_commit('-Am', 'p')::text) AS committed;
SELECT dolt_hashof('HEAD') AS p1 \gset
SET ROLE regress_rm_alice;
DROP TABLE rm.p;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'p dropped')::text) AS committed;
SELECT dolt_branch('pk') IS NOT NULL AS made;
SELECT dolt_branch('rv') IS NOT NULL AS made;
SELECT dolt_branch('q1') IS NOT NULL AS made;
SELECT dolt_branch('q2') IS NOT NULL AS made;
-- (a pick of the commit that made it)
SET ROLE regress_rm_w;
SELECT dolt_checkout('pk') IS NOT NULL AS on_pk;
SELECT length(dolt_cherry_pick(:'p1')::text) > 0 AS picked;
SELECT * FROM pg_temp.made('rm.p');
SELECT * FROM rm.p;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
-- (a revert of its drop)
SELECT dolt_checkout('rv') IS NOT NULL AS on_rv;
SELECT length(dolt_revert('HEAD')::text) > 0 AS reverted;
SELECT * FROM pg_temp.made('rm.p');
SELECT * FROM rm.p;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
RESET ROLE;
-- (a merge of a branch that restored it from its commit)
SELECT dolt_checkout('q1') IS NOT NULL AS on_q1;
SELECT dolt_checkout(:'p1', 'rm.p') IS NOT NULL AS restored;
SELECT length(dolt_commit('-Am', 'restored')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_rm_w;
SELECT dolt_checkout('q2') IS NOT NULL AS on_q2;
SELECT (dolt_merge('q1'))[2:2] AS fast_forward;
SELECT * FROM pg_temp.made('rm.p');
SELECT * FROM rm.p;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
RESET ROLE;
-- (a stash of alice's, uncommitted, applied by the writer: the stash's
-- record names its owner)
SELECT dolt_branch('al') IS NOT NULL AS made;
SELECT dolt_branch('wb') IS NOT NULL AS made;
SET ROLE regress_rm_alice;
SELECT dolt_checkout('al') IS NOT NULL AS on_al;
CREATE TABLE rm.st (id int PRIMARY KEY, note text);
INSERT INTO rm.st VALUES (1, 'stashed secret');
SELECT dolt_stash('push', 'st') IS NOT NULL AS stashed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
RESET ROLE;
SET ROLE regress_rm_w;
SELECT dolt_checkout('wb') IS NOT NULL AS on_wb;
SELECT dolt_stash('apply', 'st') IS NOT NULL AS applied;
SELECT * FROM pg_temp.made('rm.st');
SELECT * FROM rm.st;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
RESET ROLE;

-- a relation the branch has that a move changes or makes again (a table
-- whose column becomes an identity, its materialized view and view, made
-- again as a table they use changes) is changed and made as its owner
-- here, restricted, keeping its owner and grants: a superuser's merge runs
-- none of alice's code as the superuser
SET ROLE regress_rm_alice;
CREATE TABLE rm.v (id int PRIMARY KEY, b int NOT NULL, CONSTRAINT v_chk CHECK (rm_pub.f(id) > 0));
INSERT INTO rm.v VALUES (1, 1);
CREATE MATERIALIZED VIEW rm.vmv AS SELECT rm_pub.f(id) AS x FROM rm.v;
CREATE TABLE rm.other (id int PRIMARY KEY);
CREATE VIEW rm.vw AS SELECT id FROM rm.other;
GRANT SELECT ON rm.v, rm.vmv, rm.vw TO regress_rm_spy;
GRANT UPDATE (b) ON rm.v TO regress_rm_spy;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'v')::text) AS committed;
SELECT dolt_branch('y') IS NOT NULL AS made;
SELECT dolt_branch('xx') IS NOT NULL AS made;
SELECT dolt_branch('xm') IS NOT NULL AS made;
SELECT dolt_checkout('y') IS NOT NULL AS on_y;
SELECT dolt_checkout('xm') IS NOT NULL AS on_xm;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_rm_alice;
ALTER TABLE rm.other ADD COLUMN extra int;
CREATE OR REPLACE VIEW rm.vw AS SELECT id, extra FROM rm.other;
ALTER TABLE rm.v ALTER COLUMN b ADD GENERATED BY DEFAULT AS IDENTITY;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'v changed')::text) AS committed;
SELECT dolt_checkout('y') IS NOT NULL AS on_y;
CREATE TABLE rm.su (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'su on y')::text) AS committed;
TRUNCATE rm_pub.log;
SELECT (dolt_merge('main', '-m', 'merged main'))[2:2] AS fast_forward;
SELECT DISTINCT who, what FROM rm_pub.log ORDER BY 1, 2;
SELECT c.relname, c.relowner::regrole,
	has_table_privilege('regress_rm_spy', c.oid, 'SELECT') AS spy_reads
FROM pg_class c WHERE c.oid IN ('rm.v'::regclass, 'rm.vmv'::regclass, 'rm.vw'::regclass)
ORDER BY 1;
SELECT has_column_privilege('regress_rm_spy', 'rm.v', 'b', 'UPDATE') AS spy_updates_b,
	attidentity AS b_identity
FROM pg_attribute WHERE attrelid = 'rm.v'::regclass AND attname = 'b';
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- a writer that owns none of them uses a branch (its first use), and
-- merges, where alice changed her tables since: the changes are made as
-- alice
-- (the writer may read alice's definitions: the function her check calls)
GRANT USAGE ON SCHEMA rm_pub TO regress_rm_w;
SET ROLE regress_rm_w;
SELECT dolt_checkout('xx') IS NOT NULL AS on_xx;
SELECT relowner::regrole, (SELECT count(*) FROM pg_attribute WHERE attrelid = 'rm.other'::regclass AND attname = 'extra') AS has_extra
FROM pg_class WHERE oid = 'rm.other'::regclass;
SELECT dolt_checkout('xm') IS NOT NULL AS on_xm;
SELECT (dolt_merge('main'))[2:2] AS fast_forward;
SELECT relowner::regrole, (SELECT count(*) FROM pg_attribute WHERE attrelid = 'rm.other'::regclass AND attname = 'extra') AS has_extra
FROM pg_class WHERE oid = 'rm.other'::regclass;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
RESET ROLE;
REVOKE USAGE ON SCHEMA rm_pub FROM regress_rm_w;

-- a recorded owner is matched by OID and name: the role renamed (or
-- another made with its OID) refuses the move
CREATE ROLE regress_rm_ren;
GRANT USAGE, CREATE ON SCHEMA rm TO regress_rm_ren;
SET ROLE regress_rm_ren;
CREATE TABLE rm.n (id int PRIMARY KEY);
RESET ROLE;
SELECT length(dolt_commit('-Am', 'n')::text) AS committed;
SELECT dolt_hashof('HEAD') AS n1 \gset
DROP TABLE rm.n;
SELECT length(dolt_commit('-Am', 'n dropped')::text) AS committed;
ALTER ROLE regress_rm_ren RENAME TO regress_rm_ren2;
SELECT dolt_reset('--hard', :'n1');
SELECT to_regclass('rm.n') IS NULL AS not_made;
ALTER ROLE regress_rm_ren2 RENAME TO regress_rm_ren;
SELECT dolt_reset('--hard', :'n1') IS NOT NULL AS reset;
SELECT * FROM pg_temp.made('rm.n');
DROP TABLE rm.n;
SELECT length(dolt_commit('-Am', 'n dropped again')::text) AS committed;

-- the privileges --theirs saves are given back by role OID and name: a
-- grantee dropped or renamed since is passed over, with a notice, and the
-- others on the line given back
CREATE ROLE regress_rm_g1;
CREATE ROLE regress_rm_g2;
CREATE ROLE regress_rm_g3;
CREATE TABLE rm.g (id int PRIMARY KEY, note text);
INSERT INTO rm.g VALUES (1, 'g base');
SELECT length(dolt_commit('-Am', 'g')::text) AS committed;
SELECT dolt_branch('g_side') IS NOT NULL AS made;
DROP TABLE rm.g;
CREATE TABLE rm.g (id int PRIMARY KEY, note text);
GRANT SELECT ON rm.g TO regress_rm_g1, regress_rm_g2, regress_rm_g3;
SELECT length(dolt_commit('-Am', 'g again')::text) AS committed;
SELECT dolt_checkout('g_side') IS NOT NULL AS on_side;
UPDATE rm.g SET note = 'g side';
SELECT length(dolt_commit('-am', 'g side')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
BEGIN;
SELECT (dolt_merge('g_side'))[2:4] AS merged;
SELECT dolt_conflicts_resolve('--theirs', 'rm.g') IS NOT NULL AS resolved;
DROP ROLE regress_rm_g2;
ALTER ROLE regress_rm_g3 RENAME TO regress_rm_g3x;
CREATE ROLE regress_rm_g3;
SELECT (dolt_merge('--abort'))[4] AS aborted;
SELECT has_table_privilege('regress_rm_g1', 'rm.g', 'SELECT') AS g1_reads,
	has_table_privilege('regress_rm_g3', 'rm.g', 'SELECT') AS new_g3_reads,
	has_table_privilege('regress_rm_g3x', 'rm.g', 'SELECT') AS renamed_g3_reads;
COMMIT;

\c contrib_regression
DROP DATABASE regress_recorded_makers;
DROP ROLE regress_rm_alice, regress_rm_w, regress_rm_spy, regress_rm_ren,
	regress_rm_g1, regress_rm_g3, regress_rm_g3x;
