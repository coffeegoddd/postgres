# Unique checks wait for an in-progress inserter: the second insert of a
# key blocks until the first transaction ends, then fails if it committed
# and succeeds if it rolled back.  ON CONFLICT waits the same way.

setup
{
	CREATE TABLE uniq (id int PRIMARY KEY, v text);
}

teardown
{
	DROP TABLE uniq;
}

session s1
step s1_begin		{ BEGIN; }
step s1_insert		{ INSERT INTO uniq VALUES (1, 's1'); }
step s1_commit		{ COMMIT; }
step s1_rollback	{ ROLLBACK; }

session s2
step s2_insert		{ INSERT INTO uniq VALUES (1, 's2'); }
step s2_upsert		{ INSERT INTO uniq VALUES (1, 's2') ON CONFLICT (id) DO UPDATE SET v = uniq.v || '+s2'; }
step s2_select		{ SELECT * FROM uniq; }

permutation s1_begin s1_insert s2_insert s1_commit s2_select
permutation s1_begin s1_insert s2_insert s1_rollback s2_select
permutation s1_begin s1_insert s2_upsert s1_commit s2_select
