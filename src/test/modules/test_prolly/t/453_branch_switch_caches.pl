# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch switch drops only the caches that may hold another branch's
# catalog rows: the catalog caches of catalogs with a branch column, and
# the relation entries of all but system catalogs.  And invalidation
# messages that arrive while a session is switching still reach what it
# kept, and what it builds again on the new branch.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('branch_switch_caches');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE ROLE regress_bsc_role;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t VALUES (1, 'main');
	SELECT dolt_commit('-Am', 'one');
	SELECT dolt_branch('other');
});

# What a switch resets (to a branch used before): some catalog caches,
# not all of them, and no system catalog's relation entry.  (A reset of every cache, as before,
# resets each of them and the system catalogs' entries too.)
my $resets = $node->safe_psql('postgres', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('catcache-reset', 'test_prolly',
		'test_count_cache_reset', NULL);
	SELECT injection_points_attach('relcache-reset-entry', 'test_prolly',
		'test_count_cache_reset', NULL);
	SELECT (dolt_checkout('other'))[1];
	SELECT (dolt_checkout('main'))[1];
	SELECT count(*) FROM s.t;
	SELECT * FROM test_cache_resets();
	SELECT (dolt_checkout('other'))[1];
	SELECT catcaches > 0, catcaches < syscaches, system_relations
	FROM test_cache_resets();
});
is((split /\n/, $resets)[-1], 't|t|0',
	'a switch resets only the branch-aware catalog caches, and no system catalog entry');

# Messages arriving during a switch: a session on main holds at the switch
# to other (its branch set, its caches not yet reset) while another session
# changes a row every branch shares (a role's attribute: pg_authid) and a
# row of other's (a column renamed there).  Once on other, the first session
# sees both.
my $a = $node->background_psql('postgres', on_error_stop => 0);
$a->query_safe(q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('branch-switch-caches', 'wait');
	SELECT rolcanlogin FROM pg_roles WHERE rolname = 'regress_bsc_role';
	SELECT has_table_privilege('regress_bsc_role', 's.t', 'SELECT');
	SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute
	WHERE attrelid = 's.t'::regclass AND attnum > 0;
	SELECT * FROM s.t;
});
$a->query_until(qr/switching/, q{
	\echo switching
	SELECT (dolt_checkout('other'))[1];
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'branch-switch-caches'}) or die 'the switch did not wait';
$node->safe_psql('postgres', q{
	ALTER ROLE regress_bsc_role LOGIN;
});
$node->safe_psql('postgres/other', q{
	ALTER TABLE s.t RENAME COLUMN v TO w;
	GRANT SELECT ON s.t TO regress_bsc_role;
});
$node->safe_psql('postgres', q{SELECT injection_points_wakeup('branch-switch-caches')});
my $seen = $a->query_safe(q{
	SELECT injection_points_detach('branch-switch-caches');
	SELECT rolcanlogin FROM pg_roles WHERE rolname = 'regress_bsc_role';
	SELECT has_table_privilege('regress_bsc_role', 's.t', 'SELECT');
	SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute
	WHERE attrelid = 's.t'::regclass AND attnum > 0;
	SELECT w FROM s.t;
});
my @seen = split /\n/, $seen;
is($seen[-4], 't', 'the role changed during the switch is seen changed');
is($seen[-3], 't', 'the privilege granted on the new branch during the switch is seen');
is($seen[-2], 'id,w', 'the column renamed on the new branch during the switch is seen renamed');
is($seen[-1], 'main', 'its rows read through the renamed column');
$a->quit;

# And main, switched back to, is as it was.
is($node->safe_psql('postgres', q{
	SELECT (dolt_checkout('other'))[1];
	SELECT (dolt_checkout('main'))[1];
	SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute
	WHERE attrelid = 's.t'::regclass AND attnum > 0;
	SELECT has_table_privilege('regress_bsc_role', 's.t', 'SELECT');
}), "0\n0\nid,v\nf", 'main keeps its own column and privileges');

done_testing();
