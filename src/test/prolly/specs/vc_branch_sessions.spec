# Version control: sessions on different branches at the same time each
# see only their own branch, and the same key inserted on both branches
# doesn't conflict (unique checks are per branch).

setup
{
	CREATE TABLE vbs (id int PRIMARY KEY, v text);
	INSERT INTO vbs VALUES (1, 'base');
}
setup		{ SELECT prolly_commit('vbs base'); }
setup		{ SELECT prolly_branch_create('vbs_b', NULL); }

teardown
{
	DROP TABLE vbs;
}

session s1
step s1_main	{ SELECT prolly_checkout('main') IS NULL AS ok; }
step s1_insert	{ INSERT INTO vbs VALUES (2, 'main'), (5, 'main'); }
step s1_read	{ SELECT string_agg(id || ':' || v, ',' ORDER BY id) AS rows FROM vbs; }

session s2
step s2_b		{ SELECT prolly_checkout('vbs_b') IS NULL AS ok; }
step s2_insert	{ INSERT INTO vbs VALUES (3, 'b'), (5, 'b'); }
step s2_read	{ SELECT string_agg(id || ':' || v, ',' ORDER BY id) AS rows FROM vbs; }

permutation s1_main s2_b s1_insert s2_insert s1_read s2_read
