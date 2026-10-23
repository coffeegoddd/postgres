# Version control: a rewrite's checkpoint (prolly.fold_on_rewrite) never
# waits.  An ALTER that rewrote a table holds it exclusively; a commit
# open on the branch holds the branch and then reads that table.  The
# ALTER's checkpoint finds the branch held and is skipped (the table's next
# fold reads it in full), rather than waiting for the commit while the
# commit waits for the table, a deadlock.

setup		{ CREATE SCHEMA vrf VERSIONED; }
setup
{
	CREATE TABLE vrf.t (id int PRIMARY KEY, k int);
	CREATE TABLE vrf.u (id int PRIMARY KEY, w text);
	INSERT INTO vrf.t SELECT g, g FROM generate_series(1, 100) g;
	INSERT INTO vrf.u VALUES (1, 'a');
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vrf base'); END $$; }

teardown
{
	DO $$
	BEGIN
		DROP SCHEMA vrf CASCADE;
		PERFORM dolt_commit('-Am', 'vrf gone');
	END $$;
}

session s1
step s1_commit	{ BEGIN; UPDATE vrf.u SET w = 'b'; SELECT dolt_commit('-am', 'vrf u') IS NOT NULL AS ok; }
step s1_read	{ SELECT count(*) FROM vrf.t; }
step s1_end		{ COMMIT; }

session s2
step s2_alter	{ ALTER TABLE vrf.t ALTER COLUMN k TYPE bigint; }
step s2_pending	{ SELECT table_name, reason FROM dolt_pending_full_folds() WHERE table_name LIKE 'vrf.%'; }

permutation s1_commit s2_alter s1_read s1_end s2_pending
