# Copyright (c) 2026, PostgreSQL Global Development Group

# Logical decoding on a branch carries that branch's changes and the
# shared schemas'; on no branch, every branch's.  A subscription that
# connects on a branch replicates that branch's tables.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $node = ProllyTest::new_node('decoding', allows_streaming => 'logical');
$node->start;

my $b1 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b2 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');

sub on
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', "SET catalog_branch = $branch; $sql");
}

$node->safe_psql('postgres',
	'CREATE SCHEMA pub; CREATE TABLE pub.p (id int PRIMARY KEY, note text)');
for my $b ($b1, $b2)
{
	on($b,
		'CREATE SCHEMA app VERSIONED;
		 CREATE TABLE app.t (id int PRIMARY KEY, v int)');
}

$node->safe_psql('postgres',
	"SELECT 1 FROM pg_create_logical_replication_slot('branches', 'test_decoding')");
on($b1, 'INSERT INTO app.t VALUES (1, 501)');
on($b2, 'INSERT INTO app.t VALUES (1, 502)');
on($b2, "INSERT INTO pub.p VALUES (1, 'shared')");
on($b1, 'UPDATE app.t SET v = 511');
on($b2, 'TRUNCATE app.t');

my $peek = sub {
	my ($branch) = @_;
	return on($branch,
		"SELECT string_agg(substring(data from '^table ([a-z.]+: [A-Z]+)')
		 || coalesce(':' || substring(data from 'v\\[integer\\]:([0-9]+)'), ''), ','
		 ORDER BY lsn)
		 FROM pg_logical_slot_peek_changes('branches', NULL, NULL)
		 WHERE data LIKE 'table %'");
};
is($peek->($b1), 'app.t: INSERT:501,pub.p: INSERT,app.t: UPDATE:511',
	'on the first branch: its changes and the shared one');
is($peek->($b2), 'app.t: INSERT:502,pub.p: INSERT,app.t: TRUNCATE',
	'on the second branch: its changes and the shared one');
is( $peek->(0),
	'app.t: INSERT:501,app.t: INSERT:502,pub.p: INSERT,app.t: UPDATE:511,app.t: TRUNCATE',
	'on no branch: all of them');
$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('branches')");

# a publication of the second branch's table, and a subscriber in another
# database that connects on that branch
$node->safe_psql('postgres', 'CREATE DATABASE sub');
$node->safe_psql('sub', 'CREATE SCHEMA app; CREATE TABLE app.t (id int PRIMARY KEY, v int)');
on($b2, 'CREATE PUBLICATION pub2 FOR TABLE app.t');
# (a subscription in the same cluster cannot make its own slot)
$node->safe_psql('postgres',
	"SELECT 1 FROM pg_create_logical_replication_slot('s2', 'pgoutput')");
my $connstr = $node->connstr('postgres') . " options='-c catalog_branch=$b2'";
$node->safe_psql('sub',
	"CREATE SUBSCRIPTION s2 CONNECTION \$\$$connstr\$\$ PUBLICATION pub2 WITH (create_slot = false, slot_name = 's2', copy_data = false)");
$node->wait_for_subscription_sync($node, 's2', 'sub');
is($node->safe_psql('sub', 'SELECT count(*) FROM pg_subscription_rel'), 1,
	'the subscription covers the branch\'s table');
on($b1, 'INSERT INTO app.t VALUES (7001, 1)');
on($b2, 'INSERT INTO app.t VALUES (7002, 2)');
$node->wait_for_catchup('s2');
is($node->safe_psql('sub', 'SELECT string_agg(id::text, \',\' ORDER BY id) FROM app.t'),
	'7002', 'the subscriber has the second branch\'s row, not the first\'s');
$node->safe_psql('sub', 'DROP SUBSCRIPTION s2');

$node->stop;
done_testing();
