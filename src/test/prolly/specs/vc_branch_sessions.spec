# Version control: sessions on different branches at the same time each
# see only their own branch, and the same key inserted on both branches
# doesn't conflict (unique checks are per branch).

setup		{ CREATE SCHEMA vbs VERSIONED; }
setup
{
	CREATE TABLE vbs.vbs (id int PRIMARY KEY, v text);
	INSERT INTO vbs.vbs VALUES (1, 'base');
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vbs base'); END $$; }
setup		{ DO $$ BEGIN PERFORM dolt_branch('vbs_b'); END $$; }

teardown	{ DROP SCHEMA vbs CASCADE; }

session s1
step s1_main	{ SELECT dolt_checkout('main') IS NOT NULL AS ok; }
step s1_insert	{ INSERT INTO vbs.vbs VALUES (2, 'main'), (5, 'main'); }
step s1_read	{ SELECT string_agg(id || ':' || v, ',' ORDER BY id) AS rows FROM vbs.vbs; }

session s2
step s2_b		{ SELECT dolt_checkout('vbs_b') IS NOT NULL AS ok; }
step s2_insert	{ INSERT INTO vbs.vbs VALUES (3, 'b'), (5, 'b'); }
step s2_read	{ SELECT string_agg(id || ':' || v, ',' ORDER BY id) AS rows FROM vbs.vbs; }

permutation s1_main s2_b s1_insert s2_insert s1_read s2_read
