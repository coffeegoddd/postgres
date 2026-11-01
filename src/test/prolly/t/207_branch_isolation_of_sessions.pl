# Copyright (c) 2026, PostgreSQL Global Development Group

# What experiment C's catalog_branch_test.sh checked and no other test
# here does: autovacuum visits every branch's tables; large objects are
# shared; a session keeps resolving to its own branch while another
# session makes, switches and drops branches; an extension's objects of
# each branch, used in one session across a checkout, are each branch's;
# and per-table statistics counters are each branch's table's.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $node = ProllyTest::new_node('isolation_of_sessions');
$node->append_conf('postgresql.conf', qq{
autovacuum_naptime = 1s
autovacuum_vacuum_threshold = 10
autovacuum_vacuum_scale_factor = 0
autovacuum_analyze_threshold = 10
autovacuum_analyze_scale_factor = 0
});
$node->start;

my $b1 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b2 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b3 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', "SET catalog_branch = $branch; $sql");
}
for my $b ($b1, $b2)
{
	on($b,
		"CREATE SCHEMA app VERSIONED;
		 CREATE TABLE app.t (id int PRIMARY KEY, v int);
		 INSERT INTO app.t SELECT g, $b FROM generate_series(1, 100) g;
		 DELETE FROM app.t WHERE id <= 50;
		 CREATE EXTENSION hstore SCHEMA app");
}

# autovacuum visits every branch's tables (its worker is on no branch)
my ($t1, $t2) = (on($b1, "SELECT 'app.t'::regclass::oid"), on($b2, "SELECT 'app.t'::regclass::oid"));
$node->poll_query_until('postgres',
	"SELECT pg_stat_get_last_autovacuum_time($t1) IS NOT NULL AND
			pg_stat_get_last_autovacuum_time($t2) IS NOT NULL")
  or die 'autovacuum never visited both branches\' tables';
pass('autovacuum visits every branch\'s table');

# large objects are the database's, not a branch's
my $lo = on($b1, "SELECT lo_from_bytea(0, 'shared bytes')");
is(on($b2, "SELECT convert_from(lo_get($lo), 'UTF8')"), 'shared bytes',
	'a large object made on one branch is read on another');

# a session on b1 keeps resolving to b1's objects while another makes a
# schema on b3, switches branches and drops b2's catalog rows
my $s = $node->background_psql('postgres');
$s->query_safe("SET catalog_branch = $b1");
is($s->query_safe('SELECT min(v) FROM app.t'), $b1, 'a session on the first branch');
$node->safe_psql('postgres', "
	SET catalog_branch = $b3; CREATE SCHEMA app VERSIONED; CREATE TABLE app.t (x text);
	SET catalog_branch = $b2; SELECT count(*) FROM app.t;
	RESET catalog_branch; SELECT pg_branch_catalog_drop($b2);");
is($s->query_safe('SELECT min(v) FROM app.t'), $b1,
	'keeps its own branch\'s table while others make, switch and drop branches');
is($s->query_safe("SELECT count(*) FROM pg_namespace WHERE nspname = 'app'"), '1',
	'and sees its own schema alone');

# each branch's own extension, used in one session across a checkout: its
# types and operators are each branch's (no cached object of one branch is
# used on another)
on($b3, 'CREATE EXTENSION hstore SCHEMA app');
$s->query_safe("SET catalog_branch = $b3");
is($s->query_safe("SELECT 'a=>3'::app.hstore OPERATOR(app.->) 'a'"), '3', 'one branch\'s hstore');
$s->query_safe("SET catalog_branch = $b1");
is($s->query_safe("SELECT 'a=>1'::app.hstore OPERATOR(app.->) 'a'"), '1', 'then the other\'s, in one session');
is( $s->query_safe(
		"SELECT t.oid = (SELECT oid FROM pg_type WHERE typname = 'hstore') FROM pg_type t
		 WHERE t.typname = 'hstore' AND t.typnamespace = 'app'::regnamespace"),
	't', 'its type is the branch\'s own');

# per-table statistics counters are each branch's table's
on($b1, 'SELECT count(*) FROM app.t; SELECT count(*) FROM app.t;');
on($b3, 'SELECT count(*) FROM app.t');
$node->safe_psql('postgres', 'SELECT pg_stat_force_next_flush()');
my $t3 = on($b3, "SELECT 'app.t'::regclass::oid");
$node->poll_query_until('postgres', "SELECT pg_stat_get_numscans($t3) >= 1")
  or die 'the third branch\'s scan was never counted';
cmp_ok($node->safe_psql('postgres', "SELECT pg_stat_get_numscans($t1)"), '>',
	$node->safe_psql('postgres', "SELECT pg_stat_get_numscans($t3)"),
	'each branch\'s table counts its own scans');

$s->quit;
$node->stop;
done_testing();
