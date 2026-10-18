-- Doltgres's version-control function scripts (testing/go/dolt_functions_test.go
-- at doltgresql d5e05f46), run on prolly: the same objects (arrays, serials,
-- defaults, checks, foreign keys, generated columns, triggers and the
-- functions they call) through branch, checkout, commit, cherry-pick,
-- merge, conflict resolution, reset, revert, gc and log.  Each script has
-- a versioned schema of its own, first on the search path, and branches
-- named for it.  Where prolly's answer differs from Doltgres's, a comment
-- says why: commit ids are 64 hex digits (Doltgres's 32); there is no
-- staging area (dolt_add changes nothing); a database starts with no
-- commits (Doltgres's with two of its own).

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

-- The scripts' objects, made in the schema first on the search path; then
-- the second round of changes most scripts make.
CREATE FUNCTION pg_temp.dg_objects() RETURNS void LANGUAGE plpgsql AS $f$
BEGIN
	EXECUTE 'CREATE TABLE t_simple (pk INT4 PRIMARY KEY)';
	EXECUTE 'CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2))';
	EXECUTE 'CREATE TABLE t_array (pk TEXT[] PRIMARY KEY)';
	EXECUTE 'CREATE TABLE t_serial (pk SERIAL PRIMARY KEY)';
	EXECUTE 'CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8)';
	EXECUTE 'CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100))';
	EXECUTE 'CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY)';
	EXECUTE 'CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk))';
	EXECUTE 'CREATE TABLE t_unique (v1 INT4 UNIQUE)';
	EXECUTE 'CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED)';
	EXECUTE 'CREATE TABLE t_trigger (v1 INT8)';
	EXECUTE 'CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql';
	EXECUTE 'CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger()';
	EXECUTE 'CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql';
	EXECUTE 'CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8)';
	EXECUTE 'INSERT INTO t_simple VALUES (1), (2), (3)';
	EXECUTE 'INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101)';
	EXECUTE $$INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr'])$$;
	EXECUTE 'INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT)';
	EXECUTE 'INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7)';
	EXECUTE 'INSERT INTO t_checked VALUES (1), (50), (100)';
	EXECUTE 'INSERT INTO t_fk_parent VALUES (10), (20), (30)';
	EXECUTE 'INSERT INTO t_fk_child VALUES (10), (20)';
	EXECUTE 'INSERT INTO t_unique VALUES (7), (8), (9)';
	EXECUTE 'INSERT INTO t_generated (v1) VALUES (1), (2), (10)';
	EXECUTE 'INSERT INTO t_trigger (v1) VALUES (5), (10), (0)';
	EXECUTE 'INSERT INTO t_default_func (v2) VALUES (1), (2)';
END $f$;
CREATE FUNCTION pg_temp.dg_changes() RETURNS void LANGUAGE plpgsql AS $f$
BEGIN
	EXECUTE 'INSERT INTO t_simple VALUES (4)';
	EXECUTE 'INSERT INTO t_composite VALUES (3, 100)';
	EXECUTE $$INSERT INTO t_array VALUES (ARRAY['stu'])$$;
	EXECUTE 'INSERT INTO t_serial VALUES (DEFAULT)';
	EXECUTE 'INSERT INTO t_default_simple VALUES (98, 8)';
	EXECUTE 'INSERT INTO t_checked VALUES (99)';
	EXECUTE 'INSERT INTO t_fk_parent VALUES (40)';
	EXECUTE 'INSERT INTO t_fk_child VALUES (30)';
	EXECUTE 'INSERT INTO t_unique VALUES (10)';
	EXECUTE 'INSERT INTO t_generated (v1) VALUES (11)';
	EXECUTE 'CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql';
	EXECUTE 'INSERT INTO t_trigger (v1) VALUES (2)';
	EXECUTE 'CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql';
	EXECUTE 'INSERT INTO t_default_func (v2) VALUES (3)';
END $f$;
-- (each script's commits, checked to make one)
CREATE FUNCTION pg_temp.committed(VARIADIC args text[]) RETURNS bool LANGUAGE sql AS $$
	SELECT length((dolt_commit(VARIADIC args))[1]) = 64
$$;
-- dolt_status's rows for schema s
CREATE FUNCTION pg_temp.status_of(s text) RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM dolt_status WHERE table_name LIKE s || '.%'
$$;

-- TestDoltCommit "Stage all then commit", "Commit with staging all".
-- dolt_status lists each table made, and each function (by its argument
-- types) and sequence: 15 rows, where Doltgres's 16 also list the trigger,
-- an object of its own there (prolly's is part of its table's
-- definition).  (The schema is committed first, as Doltgres's public is.)
CREATE SCHEMA dg_commit VERSIONED;
SELECT pg_temp.committed('-A', '-m', 'dg_commit made');
SET search_path = dg_commit;
SELECT pg_temp.dg_objects();
SELECT pg_temp.status_of('dg_commit') AS status_rows;
SELECT dolt_add('-A');
SELECT pg_temp.committed('-m', 'initial');
SELECT pg_temp.status_of('dg_commit') AS status_rows;
-- "Allow and skip empty"
INSERT INTO t_simple VALUES (4);
SELECT pg_temp.committed('-A', '-m', 'initial');
SELECT dolt_commit('-m', 'should_error');
SELECT dolt_commit('--skip-empty', '-m', 'should_error');
SELECT pg_temp.committed('--allow-empty', '-m', 'initial');
RESET search_path;

-- TestDoltBranch "All branch options"
CREATE SCHEMA dg_branch VERSIONED;
SET search_path = dg_branch;
SELECT pg_temp.dg_objects();
SELECT dolt_add('-A');
SELECT pg_temp.committed('-m', 'initial');
SELECT dolt_branch('dg_original');
INSERT INTO t_simple VALUES (4);
SELECT dolt_add('-A');
SELECT pg_temp.committed('-m', 'initial');
SELECT dolt_branch('-c', 'main', 'dg_copy');
SELECT dolt_branch('-c', '-f', 'dg_original', 'dg_forcecopy');
SELECT * FROM t_simple;
SELECT dolt_checkout('main');
SELECT dolt_checkout('dg_original');
SELECT * FROM t_simple;
SELECT dolt_checkout('dg_copy');
SELECT * FROM t_simple;
SELECT dolt_checkout('dg_forcecopy');
SELECT * FROM t_simple;
SELECT dolt_branch('-d', 'dg_forcecopy');
SELECT dolt_branch('-d', 'dg_original');
SELECT dolt_branch('-m', 'dg_copy', 'dg_renamedcopy');
SELECT dolt_checkout('dg_original');
SELECT dolt_checkout('dg_copy');
SELECT dolt_checkout('dg_renamedcopy');
SELECT * FROM t_simple;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dg_forcecopy', 'dg_renamedcopy');
RESET search_path;

-- TestDoltCheckout "All checkout options", TestDoltCherryPick "Smoke test",
-- TestDoltRevert "Revert a single commit": the objects through a second
-- round of changes, functions redefined among them.
CREATE SCHEMA dg_checkout VERSIONED;
SET search_path = dg_checkout;
SELECT pg_temp.dg_objects();
SELECT dolt_add('-A');
SELECT pg_temp.committed('-m', 'initial');
SELECT dolt_branch('dg_co_original');
SELECT pg_temp.dg_changes();
SELECT dolt_add('-A');
SELECT pg_temp.committed('-m', 'initial');
SELECT dolt_checkout('-b', 'dg_checkoutbranch');
SELECT * FROM t_simple;
SELECT dolt_checkout('dg_checkoutbranch');
SELECT dolt_checkout('main');
SELECT * FROM t_simple;
SELECT dolt_checkout('dg_co_original');
SELECT * FROM t_simple;
-- (the cherry-pick: main's head onto the original branch)
SELECT (dolt_cherry_pick('main'))[2:4];
SELECT * FROM t_simple;
SELECT f_default(), (SELECT v1 FROM t_default_func WHERE v2 = 3) AS made_with_34;
INSERT INTO t_trigger VALUES (1);
SELECT v1 FROM t_trigger ORDER BY v1;
-- (the revert, on main: its head's changes undone, the functions too)
SELECT dolt_checkout('main');
SELECT * FROM t_simple;
SELECT f_default();
SELECT (dolt_revert('HEAD'))[2:4];
SELECT * FROM t_simple;
SELECT f_default();
SELECT dolt_branch('-D', 'dg_co_original', 'dg_checkoutbranch');
RESET search_path;

-- TestDoltConflictsResolve "Simple conflicts resolve ...": all with
-- --ours, all with --theirs, and the table with --ours (Doltgres skips its
-- table with --theirs; run here too)
CREATE SCHEMA dg_resolve VERSIONED;
SET search_path = dg_resolve;
CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);
INSERT INTO t_simple VALUES (1, 1);
SELECT pg_temp.committed('-Am', 'initial');
SELECT dolt_branch('dg_other');
INSERT INTO t_simple VALUES (2, 2);
SELECT pg_temp.committed('-Am', 'second row on main');
SELECT dolt_checkout('dg_other');
INSERT INTO t_simple VALUES (2, 3);
SELECT pg_temp.committed('-Am', 'conflicted row on other');
SELECT dolt_checkout('main');
BEGIN;
SELECT dolt_merge('dg_other', '--no-ff');
SELECT * FROM dolt_conflicts;
SELECT dolt_conflicts_resolve('--ours', '.');
SELECT pg_temp.committed('-Am', 'commit merge');
SELECT * FROM t_simple ORDER BY pk;
ROLLBACK;
BEGIN;
SELECT dolt_merge('dg_other', '--no-ff');
SELECT dolt_conflicts_resolve('--theirs', '.');
SELECT pg_temp.committed('-Am', 'commit merge');
SELECT * FROM t_simple ORDER BY pk;
ROLLBACK;
BEGIN;
SELECT dolt_merge('dg_other', '--no-ff');
SELECT dolt_conflicts_resolve('--ours', 'dg_resolve.t_simple');
SELECT pg_temp.committed('-Am', 'commit merge');
SELECT * FROM t_simple ORDER BY pk;
ROLLBACK;
BEGIN;
SELECT dolt_merge('dg_other', '--no-ff');
SELECT dolt_conflicts_resolve('--theirs', 'dg_resolve.t_simple');
SELECT pg_temp.committed('-Am', 'commit merge');
SELECT * FROM t_simple ORDER BY pk;
ROLLBACK;
SELECT dolt_branch('-D', 'dg_other');
RESET search_path;

-- TestDoltGC "Full", "Shallow"; TestDoltLog "Smoke test" (Doltgres counts
-- its database's two commits of its own: 4 and 3)
CREATE SCHEMA dg_gc VERSIONED;
SET search_path = dg_gc;
SELECT pg_temp.dg_objects();
SELECT pg_temp.committed('-A', '-m', 'initial');
SELECT dolt_branch('dg_gc_original');
SELECT pg_temp.dg_changes();
SELECT pg_temp.committed('-A', '-m', 'initial');
SELECT dolt_gc();
SELECT dolt_gc('--shallow');
SELECT (SELECT count(*) FROM dolt_log('main')) - (SELECT count(*) FROM dolt_log('dg_gc_original'))
	AS main_ahead;
SELECT dolt_branch('-D', 'dg_gc_original');
RESET search_path;

-- TestDoltMerge "Merge without conflicts", "Fast forward", "--no-ff",
-- "Merge with nested parentheses in default, generated, and check
-- expressions"
CREATE SCHEMA dg_merge VERSIONED;
SET search_path = dg_merge;
CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);
CREATE TABLE t_composite (pk1 INT2, pk2 INT8, v1 INT4, PRIMARY KEY(pk1, pk2));
CREATE TABLE t_array (pk TEXT[] PRIMARY KEY, v1 INT4);
CREATE TABLE t_serial (pk SERIAL PRIMARY KEY, v1 INT4);
CREATE TABLE t_generated (pk INT8 PRIMARY KEY, v1 INT4, v2 INT8 GENERATED ALWAYS AS (pk * 1000) STORED);
CREATE TABLE t_trigger (pk INT4 PRIMARY KEY, v1 INT8);
CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;
CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();
CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;
CREATE TABLE t_default_func (pk INT4 PRIMARY KEY, v1 INT8 DEFAULT f_default(), v2 INT8);
INSERT INTO t_simple VALUES (1, 1);
INSERT INTO t_composite VALUES (1, 1, 1);
INSERT INTO t_array VALUES (ARRAY['abc'], 1);
INSERT INTO t_serial VALUES (DEFAULT, 1);
INSERT INTO t_generated (pk, v1) VALUES (1, 1);
INSERT INTO t_trigger VALUES (1, 1);
INSERT INTO t_default_func (pk, v2) VALUES (1, 1);
SELECT pg_temp.committed('-A', '-m', 'initial');
SELECT dolt_branch('dg_m_other');
INSERT INTO t_simple VALUES (2, 2);
INSERT INTO t_composite VALUES (2, 2, 2);
INSERT INTO t_array VALUES (ARRAY['def'], 2);
INSERT INTO t_serial VALUES (DEFAULT, 2);
INSERT INTO t_generated (pk, v1) VALUES (2, 2);
CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;
INSERT INTO t_trigger VALUES (2, 2);
CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;
INSERT INTO t_default_func (pk, v2) VALUES (2, 2);
SELECT pg_temp.committed('-A', '-m', 'initial');
SELECT dolt_checkout('dg_m_other');
INSERT INTO t_simple VALUES (3, 3);
INSERT INTO t_composite VALUES (3, 2, 3);
INSERT INTO t_array VALUES (ARRAY['dfe'], 3);
INSERT INTO t_serial VALUES (4, 3);
INSERT INTO t_generated (pk, v1) VALUES (3, 3);
INSERT INTO t_trigger VALUES (3, 3);
CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;
INSERT INTO t_default_func (pk, v2) VALUES (3, 3);
SELECT pg_temp.committed('-A', '-m', 'initial');
SELECT strpos(dolt_merge('main')::text, '0,0,"merge successful"') > 1 AS merged;
SELECT * FROM t_simple ORDER BY pk;
SELECT f_default(), (SELECT v1 FROM t_trigger WHERE pk = 3) AS other_trigger_row;
-- (main's f_default merged in, the other's unchanged from base: 34)
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dg_m_other');
RESET search_path;

CREATE SCHEMA dg_ff VERSIONED;
SET search_path = dg_ff;
SELECT pg_temp.dg_objects();
SELECT pg_temp.committed('-A', '-m', 'initial');
SELECT dolt_branch('dg_ff_original');
SELECT dolt_branch('dg_noff_original');
SELECT pg_temp.dg_changes();
SELECT pg_temp.committed('-A', '-m', 'initial');
SELECT dolt_checkout('dg_ff_original');
SELECT strpos(dolt_merge('main')::text, '1,0,"merge successful"') > 1 AS fast_forward;
SELECT dolt_checkout('dg_noff_original');
SELECT strpos(dolt_merge('main', '--no-ff', '-m', 'merge_commit')::text, 'merge successful') > 1 AS merged;
SELECT count(*) FROM t_simple;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dg_ff_original', 'dg_noff_original');
RESET search_path;

CREATE SCHEMA dg_nested VERSIONED;
SET search_path = dg_nested;
CREATE TABLE t3324 (pk INT4 PRIMARY KEY, a INT4 DEFAULT (1 + 1) * 2, b INT4 GENERATED ALWAYS AS ((pk + 1) * 2) STORED, CONSTRAINT c3324 CHECK (NOT (pk = 0 OR pk + 1 = 0) AND ((pk + 1) * 2) > 3));
INSERT INTO t3324 (pk) VALUES (1);
SELECT pg_temp.committed('-A', '-m', 'initial');
SELECT dolt_branch('dg_n_other');
INSERT INTO t3324 (pk) VALUES (2);
SELECT pg_temp.committed('-A', '-m', 'main');
SELECT dolt_checkout('dg_n_other');
INSERT INTO t3324 (pk) VALUES (3);
SELECT pg_temp.committed('-A', '-m', 'other');
SELECT strpos(dolt_merge('main', '--no-ff', '-m', 'merge_commit')::text, 'merge successful') > 1 AS merged;
SELECT * FROM t3324 ORDER BY pk;
INSERT INTO t3324 (pk) VALUES (0);
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dg_n_other');
RESET search_path;

-- TestDoltReset "Hard": the uncommitted objects gone (Doltgres skips its
-- check of dolt_status after: prolly's is empty)
CREATE SCHEMA dg_reset VERSIONED;
SELECT pg_temp.committed('-A', '-m', 'dg_reset made');
SET search_path = dg_reset;
SELECT pg_temp.dg_objects();
SELECT pg_temp.status_of('dg_reset') AS status_rows;
SELECT dolt_reset('--hard', 'HEAD');
SELECT pg_temp.status_of('dg_reset') AS status_rows;
SELECT count(*) AS tables_left FROM pg_tables WHERE schemaname = 'dg_reset';
RESET search_path;

-- TestDoltFunctionSmokeTests "smoke test select dolt_merge", "... dirty
-- working set, same table", "... dirty working set, different tables"
CREATE SCHEMA dg_smoke VERSIONED;
SET search_path = dg_smoke;
CREATE TABLE t1 (pk int PRIMARY KEY);
SELECT pg_temp.committed('-Am', 'new table');
SELECT dolt_checkout('-b', 'dg-new-branch');
CREATE TABLE t2 (pk int PRIMARY KEY);
SELECT pg_temp.committed('-Am', 'new table on new branch');
SELECT dolt_merge_base('main', 'dg-new-branch') = dolt_hashof('main') AS base_is_main;
SELECT dolt_checkout('main');
SELECT dolt_hashof('main') AS before_merge \gset
SELECT strpos(dolt_merge('dg-new-branch', '--no-ff', '-m', 'merge new-branch into main')::text, 'merge successful') > 1 AS merged;
SELECT count(*) AS new_commits FROM dolt_log('main') l
 WHERE l.commit_hash NOT IN (SELECT commit_hash FROM dolt_log(:'before_merge'));
-- dirty, the same table: refused
INSERT INTO t1 VALUES (1);
SELECT dolt_checkout('-b', 'dg-new-branch2');
INSERT INTO t1 VALUES (2);
SELECT pg_temp.committed('-Am', 'new row on new branch');
SELECT dolt_checkout('main');
SELECT * FROM dolt_status WHERE table_name LIKE 'dg_smoke.%';
SELECT dolt_merge('dg-new-branch2', '--no-ff', '-m', 'merge new-branch into main');
SELECT * FROM dolt_status WHERE table_name LIKE 'dg_smoke.%';
-- dirty, another table: Doltgres merges, the change left uncommitted;
-- prolly refuses a merge that changes definitions (here, makes a table)
-- over uncommitted changes, which stay
SELECT dolt_checkout('-b', 'dg-new-branch3', 'HEAD');
CREATE TABLE t3 (pk int PRIMARY KEY);
SELECT pg_temp.committed('-Am', 'new table on new branch');
SELECT dolt_checkout('main');
SELECT dolt_merge('dg-new-branch3', '--no-ff', '-m', 'merge new-branch into main');
SELECT * FROM dolt_status WHERE table_name LIKE 'dg_smoke.%';
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'dg-new-branch', 'dg-new-branch2', 'dg-new-branch3');
RESET search_path;

-- TestDoltDiff "IN expressions over table function commit columns": the
-- log's commit ids compared with IN
SELECT count(*) FROM dolt_log('HEAD') WHERE commit_hash IN (dolt_hashof('HEAD'), dolt_hashof('HEAD~1'));
SELECT count(*) FROM dolt_log('HEAD') WHERE commit_hash IN ('not_a_hash');

SET client_min_messages = warning;
DROP SCHEMA dg_commit, dg_branch, dg_checkout, dg_resolve, dg_gc, dg_merge, dg_ff,
	dg_nested, dg_reset, dg_smoke CASCADE;
RESET client_min_messages;
SELECT pg_temp.committed('-Am', 'doltgres scripts done');
