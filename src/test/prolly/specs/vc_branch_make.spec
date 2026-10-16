# Version control: making a branch holds up the source branch's writers
# only while its copies are made, not until the making transaction ends;
# the rows written meanwhile stay the source's.

setup		{ CREATE SCHEMA vbm VERSIONED; }
setup
{
	CREATE TABLE vbm.t (id int PRIMARY KEY, v text);
	CREATE TABLE vbm.k (v text);
	INSERT INTO vbm.t VALUES (1, 'base');
	INSERT INTO vbm.k VALUES ('base');
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vbm base'); END $$; }

teardown
{
	DO $$ BEGIN PERFORM dolt_branch('-D', 'vbm_b'); PERFORM dolt_reset('--hard'); END $$;
	DROP SCHEMA vbm CASCADE;
}

session s1
step s1_begin	{ BEGIN; }
step s1_branch	{ SELECT dolt_branch('vbm_b'); }
step s1_commit	{ COMMIT; }
step s1_on_b	{ SELECT dolt_checkout('vbm_b') IS NOT NULL AS ok; }
step s1_read	{ SELECT (SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM vbm.t) AS t,
				  (SELECT string_agg(v, ',' ORDER BY v) FROM vbm.k) AS k; }
step s1_main	{ SELECT dolt_checkout('main') IS NOT NULL AS ok; }

session s2
step s2_write	{ INSERT INTO vbm.t VALUES (2, 'main'); INSERT INTO vbm.k VALUES ('main'); }
step s2_read	{ SELECT (SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM vbm.t) AS t,
				  (SELECT string_agg(v, ',' ORDER BY v) FROM vbm.k) AS k; }

permutation s1_begin s1_branch s2_write s1_commit s1_on_b s1_read s1_main s2_read
