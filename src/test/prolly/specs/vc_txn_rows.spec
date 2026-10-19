# Dolt's multi-session row tests (DoltTransactionTests 0-12,
# DoltConflictHandlingTests 0-2 and 7-8, DoltConstraintViolationTransaction-
# Tests 0), rederived under Postgres semantics.  Dolt's sessions each write
# their own working set and merge it into the branch's at COMMIT, cell by
# cell, failing the later commit on a conflict.  Two sessions on one
# prolly branch write one set of tables, under Postgres's MVCC and row
# locks: a second writer of a row waits for the first; in READ COMMITTED
# it then works on the row as committed (so updates of different columns
# of a row both land, as Dolt's cell merge has it), and an insert of a key
# the first committed fails; in REPEATABLE READ the second fails with a
# serialization failure, where Dolt's later commit does.

setup		{ CREATE SCHEMA vtr VERSIONED; }
setup
{
	CREATE TABLE vtr.t (x int PRIMARY KEY, y int, z int);
	INSERT INTO vtr.t VALUES (1, 1, 1), (2, 2, 2);
	CREATE TABLE vtr.parent (pk int PRIMARY KEY);
	CREATE TABLE vtr.child (pk int PRIMARY KEY, p int REFERENCES vtr.parent);
	INSERT INTO vtr.parent VALUES (1);
	CREATE TABLE vtr.keyless (c int);
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vtr base'); END $$; }

teardown
{
	DO $$
	BEGIN
		DROP SCHEMA vtr CASCADE;
		PERFORM dolt_commit('-Am', 'vtr gone');
	END $$;
}

session a
step a_begin	{ BEGIN; }
step a_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step a_ins2		{ INSERT INTO vtr.t VALUES (3, 3, 3); }
step a_upd_y	{ UPDATE vtr.t SET y = 10 WHERE x = 1; }
step a_upd_all_y { UPDATE vtr.t SET y = 2; }
step a_del1		{ DELETE FROM vtr.t WHERE x = 1; }
step a_del_parent { DELETE FROM vtr.parent WHERE pk = 1; }
step a_keyless	{ INSERT INTO vtr.keyless VALUES (1); }
step a_commit	{ COMMIT; }

session b
step b_begin	{ BEGIN; }
step b_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step b_allow	{ SET LOCAL dolt.allow_commit_conflicts = on; }
step b_ins2		{ INSERT INTO vtr.t VALUES (3, 3, 3); }
step b_ins2_other { INSERT INTO vtr.t VALUES (3, 30, 30); }
step b_upd_y	{ UPDATE vtr.t SET y = 10 WHERE x = 1; }
step b_upd_y_other { UPDATE vtr.t SET y = 20 WHERE x = 1; }
step b_upd_y2	{ UPDATE vtr.t SET y = 20 WHERE x = 2; }
step b_upd_all_z { UPDATE vtr.t SET z = 3; }
step b_del1		{ DELETE FROM vtr.t WHERE x = 1; }
step b_del2		{ DELETE FROM vtr.t WHERE x = 2; }
step b_ins1		{ INSERT INTO vtr.t VALUES (1, 5, 5); }
step b_ins_child { INSERT INTO vtr.child VALUES (1, 1); }
step b_keyless	{ INSERT INTO vtr.keyless VALUES (1), (1); }
step b_commit	{ COMMIT; }
step b_rollback	{ ROLLBACK; }
step b_read		{ SELECT string_agg(x || ':' || y || ':' || z, ',' ORDER BY x) AS rows FROM vtr.t; }
step b_keyless_read { SELECT count(*) AS copies FROM vtr.keyless; }
step b_status	{ SELECT count(*) AS changed FROM dolt_status WHERE table_name LIKE 'vtr.%'; }
step b_violations { SELECT count(*) AS violations FROM dolt_constraint_violations() WHERE table_name LIKE 'vtr.%'; }

# duplicate inserts, autocommit on (DoltTransactionTests 0): the second
# insert fails at once, the key taken
permutation a_ins2 b_ins2 b_read b_status
# duplicate (1) and conflicting (2, DoltConflictHandlingTests 0-2) inserts
# in transactions: the second waits, then fails on the key the first
# committed (Dolt: the identical insert merges; the different one fails
# the later commit)
permutation a_begin b_begin a_ins2 b_ins2 a_commit b_rollback b_read
permutation a_begin b_begin a_ins2 b_ins2_other a_commit b_rollback b_read
# the same with dolt.allow_commit_conflicts on (DoltConflictHandlingTests
# 1-2): no commit-time merge keeps a conflict, the setting changes nothing
permutation a_begin b_begin b_allow a_ins2 b_ins2_other a_commit b_rollback b_read
# duplicate updates, autocommit on (3): each runs alone, the second
# finding the row as the first left it
permutation a_upd_y b_upd_y b_read
# duplicate (4) and conflicting (5) updates: the second waits, then
# updates the row as committed (READ COMMITTED); in REPEATABLE READ, a
# serialization failure, as Dolt's later commit's conflict
permutation a_begin b_begin a_upd_y b_upd_y a_commit b_commit b_read
permutation a_begin b_begin a_upd_y b_upd_y_other a_commit b_commit b_read
permutation a_rr b_rr a_upd_y b_upd_y_other a_commit b_rollback b_read
# non-overlapping updates, different rows (6): neither waits
permutation a_begin b_begin a_upd_y b_upd_y2 a_commit b_commit b_read
# different columns of the same rows (7): the second waits, then sets its
# column on the rows as committed: both land, as Dolt's cell merge has it
permutation a_begin b_begin a_upd_all_y b_upd_all_z a_commit b_commit b_read
# duplicate deletes, autocommit on (8): the second deletes nothing
permutation a_del1 b_del1 b_read
# duplicate (9) and non-overlapping (10) deletes; a delete and an update
# of the row (11): the second waits, then finds no row
permutation a_begin b_begin a_del1 b_del1 a_commit b_commit b_read
permutation a_begin b_begin a_del1 b_del2 a_commit b_commit b_read
permutation a_begin b_begin a_del1 b_upd_y a_commit b_commit b_read
# a delete in one, the key inserted again in another (12): the insert
# waits on the delete, then succeeds
permutation a_begin b_begin a_del1 b_ins1 a_commit b_commit b_read
# Dolt's first write wins (DoltConflictHandlingTests 7, 8): here the
# second writer of a keyed row waits, then overwrites the row as committed
# (the last write wins in READ COMMITTED; REPEATABLE READ fails, as
# above), whichever session commits first; a keyless table's inserts are
# rows of their own, all kept
permutation a_begin b_begin a_keyless b_keyless a_upd_y b_upd_y_other a_commit b_commit b_read b_keyless_read
permutation a_begin b_begin b_upd_y_other a_upd_y b_commit a_commit b_read
# violations by concurrent writes (DoltConstraintViolationTransactionTests
# 0): Postgres's foreign key locks the parent row, so the child's insert
# waits on the parent's delete, then fails; no violation is left to record
permutation a_begin b_begin a_del_parent b_ins_child a_commit b_rollback b_status b_violations
