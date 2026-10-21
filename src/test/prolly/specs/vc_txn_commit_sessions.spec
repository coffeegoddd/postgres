# Version control, dolt.transaction_commit with many sessions: Dolt's
# transaction-commit tests (one client in the mode, two with a third
# outside it, autocommit), rederived, and more writers than two, at each
# isolation level.  Every committed transaction in the mode makes one
# commit, in the order the transactions commit, holding exactly its own
# rows, on the head as it is when it commits; rows written outside the
# mode stay in the working set, out of the commits.  Its only
# serialization failures are the heap's own, and a transaction failing
# one leaves no commit.

setup		{ CREATE SCHEMA vts VERSIONED; }
setup		{ CREATE TABLE vts.x (y int PRIMARY KEY, z int); INSERT INTO vts.x VALUES (1, 1); }
setup
{
	CREATE TABLE vts.parent (pk int PRIMARY KEY);
	CREATE TABLE vts.child (pk int PRIMARY KEY, v1 int REFERENCES vts.parent);
	CREATE INDEX ON vts.child (v1);
	INSERT INTO vts.parent VALUES (1), (2);
	CREATE TABLE vts.emails (id text PRIMARY KEY, mailbox_id text, html_body text);
	CREATE INDEX ON vts.emails USING gin (to_tsvector('simple', html_body));
	INSERT INTO vts.emails
		SELECT 'seed-' || g, 'inbox', 'hello world unique' || g || ' content here'
		  FROM generate_series(1, 500) g;
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vts base'); END $$; }

teardown
{
	DO $$
	BEGIN
		DROP SCHEMA vts CASCADE;
		PERFORM dolt_commit('-Am', 'vts gone');
	END $$;
}

session a
setup			{ SET dolt.transaction_commit = on; SET dolt.transaction_commit_message = 'a'; }
step a_rc		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step a_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step a_ser		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step a_read		{ SELECT count(*) FROM vts.x; }
step a_insert	{ INSERT INTO vts.x VALUES (2, 2); }
step a_update	{ UPDATE vts.x SET z = z + 10 WHERE y = 1; }
step a_rows		{ SELECT string_agg(y || ':' || z, ',' ORDER BY y) AS a_sees FROM vts.x; }
step a_table	{ SET dolt.transaction_commit_message = 'Commit Message 42'; CREATE TABLE vts.new_table (pk int PRIMARY KEY); }
step a_child	{ INSERT INTO vts.child VALUES (1, 1); }
step a_bad_child	{ INSERT INTO vts.child VALUES (3, 3); }
step a_children	{ SELECT pk, v1 FROM vts.child WHERE v1 > 0 ORDER BY pk; }
step a_email	{ INSERT INTO vts.emails (id, mailbox_id) VALUES ('tx-a', 'box-a'); }
step a_emails
{
	SELECT (SELECT count(*) FROM vts.emails) AS emails,
		   (SELECT count(*) FROM vts.emails
			 WHERE to_tsvector('simple', html_body) @@ to_tsquery('simple', 'hello')) AS hello,
		   (SELECT count(*) FROM dolt_at('HEAD', NULL::vts.emails)) AS at_head;
}
step a_prepare	{ PREPARE TRANSACTION 'vts_a'; }
step a_commit	{ COMMIT; }

session b
setup			{ SET dolt.transaction_commit = on; SET dolt.transaction_commit_message = 'b'; }
step b_begin	{ BEGIN; }
step b_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step b_ser		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step b_outside	{ SET dolt.transaction_commit = off; }
step b_read		{ SELECT count(*) FROM vts.x; }
step b_insert	{ INSERT INTO vts.x VALUES (3, 3); }
step b_update	{ UPDATE vts.x SET z = z + 100 WHERE y = 1; }
step b_rows		{ SELECT string_agg(y || ':' || z, ',' ORDER BY y) AS b_sees FROM vts.x; }
step b_child	{ INSERT INTO vts.child VALUES (2, 2); }
step b_email	{ INSERT INTO vts.emails (id, mailbox_id) VALUES ('tx-b', 'box-b'); }
step b_message	{ SET dolt.transaction_commit_message = 'ClientB Commit'; }
step b_prepare	{ PREPARE TRANSACTION 'vts_b'; }
step b_finish	{ COMMIT PREPARED 'vts_b'; }
step b_commit	{ COMMIT; }

# (outside the mode, in autocommit)
session c
step c_insert	{ INSERT INTO vts.x VALUES (4, 4); }
step c_log
{
	SELECT string_agg(message, ',' ORDER BY commit_order DESC) AS log
	  FROM dolt_log('main') WHERE commit_order > (SELECT max(commit_order) FROM dolt_log('main') WHERE message = 'vts base');
}
# each commit's own rows: what it changed from its parent
step c_diffs
{
	SELECT string_agg(l.message || '[' || coalesce((
			SELECT string_agg(d.diff_type || ' ' ||
				coalesce((d.to_row).y, (d.from_row).y) || ':' ||
				coalesce((d.to_row).z::text, '-'), ', '
				ORDER BY coalesce((d.to_row).y, (d.from_row).y))
			  FROM dolt_diff(l.commit_hash || '~1', l.commit_hash, NULL::vts.x) d), '') || ']',
		' ' ORDER BY l.commit_order) AS diffs
	  FROM dolt_log('main') l
	 WHERE l.commit_order > (SELECT max(commit_order) FROM dolt_log('main') WHERE message = 'vts base');
}
step c_prepared	{ SELECT gid FROM pg_prepared_xacts WHERE gid LIKE 'vts%' ORDER BY gid; }
step c_heads
{
	SELECT (SELECT string_agg(y || ':' || z, ',' ORDER BY y) FROM dolt_at('HEAD', NULL::vts.x)) AS head,
		   (SELECT string_agg(y || ':' || z, ',' ORDER BY y) FROM vts.x) AS working,
		   (SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::vts.x)) AS uncommitted;
}

session d
setup			{ SET dolt.transaction_commit = on; SET dolt.transaction_commit_message = 'd'; }
step d_begin	{ BEGIN; }
step d_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step d_ser		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step d_insert	{ INSERT INTO vts.x VALUES (5, 5); }
step d_update	{ UPDATE vts.x SET z = z + 1000 WHERE y = 1; }
step d_commit	{ COMMIT; }

session e
setup			{ SET dolt.transaction_commit = on; SET dolt.transaction_commit_message = 'e'; }
step e_begin	{ BEGIN; }
step e_insert	{ INSERT INTO vts.x VALUES (6, 6); }
step e_delete	{ DELETE FROM vts.x WHERE y = 1; }
step e_commit	{ COMMIT; }

# Dolt's one client: a commits in the mode; b, outside it, commits first,
# its row left in the working set.  A transaction that changes definitions
# commits the branch's working set, b's row with it (Dolt's holds what was
# staged).
permutation a_rc b_begin b_outside a_insert b_insert a_rows b_rows b_commit a_commit c_log c_heads a_table c_log c_heads

# Dolt's two clients: c writes outside the mode first; b then a commit,
# each its own row; c's stays uncommitted.
permutation c_insert a_rc b_begin a_insert b_insert a_rows b_rows b_message b_commit a_commit c_log c_diffs c_heads

# Dolt's autocommit: each statement in the mode is a commit.
permutation b_message a_insert b_insert c_log c_heads

# Four writers at read committed, on overlapping rows: those updating the
# row a updated wait for it, then add to it as committed.
permutation a_rc b_begin d_begin e_begin a_update b_insert d_update e_insert a_commit d_commit b_update e_delete b_commit e_commit c_log c_diffs c_heads

# The same at repeatable read: a writer of the row another committed fails,
# the heap's own failure, and leaves no commit; the rest commit on the
# head as it is, whatever their snapshots saw.
permutation a_rr b_rr d_rr a_update b_insert d_update a_commit d_commit b_commit c_log c_diffs c_heads

# Serializable: disjoint writers all commit ...
permutation a_ser b_ser d_ser a_insert b_insert d_insert d_commit b_commit a_commit c_log c_diffs c_heads

# ... and two that each read what the other wrote fail the later one's
# COMMIT, the heap's own failure:
# its commit, made as it committed, goes with it.
permutation a_ser b_ser a_read b_read a_insert b_insert a_commit b_commit c_log c_diffs c_heads

# Dolt's late foreign-key resolution: two writers of child rows both
# commit; a child row with no parent fails and makes no commit.
permutation a_rc b_begin a_child b_child a_commit b_commit a_children a_bad_child c_log

# Dolt's null-indexed-column inserts: two writers of rows a GIN index's
# expression is null for, the later committing on the head the earlier
# moved; every row is there and the index finds the seeded ones.
permutation a_rc b_begin a_email b_email b_commit a_commit a_emails c_log

# A transaction the mode would commit can't be prepared (it is rolled
# back); one that changed no branch can.
permutation a_rc a_insert a_prepare b_begin b_read b_prepare c_prepared b_finish c_log c_heads
