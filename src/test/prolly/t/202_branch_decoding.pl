# Copyright (c) 2026, PostgreSQL Global Development Group

# Logical decoding from a slot made on a branch carries that branch's
# changes and the shared schemas'; from one made on no branch, every
# branch's.  The branch is the slot's, whichever branch the session reading
# it is on, so a subscription whose slot was made on a branch replicates
# that branch's tables.

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

# a slot made on each branch, and one on no branch; the branch is the
# slot's, kept with it, whichever the session reading it is on
on($b1, "SELECT 1 FROM pg_create_logical_replication_slot('on_b1', 'test_decoding')");
on($b2, "SELECT 1 FROM pg_create_logical_replication_slot('on_b2', 'test_decoding')");
$node->safe_psql('postgres',
	"SELECT 1 FROM pg_create_logical_replication_slot('on_none', 'test_decoding')");
on($b1, 'INSERT INTO app.t VALUES (1, 501)');
on($b2, 'INSERT INTO app.t VALUES (1, 502)');
on($b2, "INSERT INTO pub.p VALUES (1, 'shared')");
on($b1, 'UPDATE app.t SET v = 511');
on($b2, 'TRUNCATE app.t');

my $peek = sub {
	my ($slot, $branch) = @_;
	return on($branch,
		"SELECT string_agg(substring(data from '^table ([a-z.]+: [A-Z]+)')
		 || coalesce(':' || substring(data from 'v\\[integer\\]:([0-9]+)'), ''), ','
		 ORDER BY lsn)
		 FROM pg_logical_slot_peek_changes('$slot', NULL, NULL)
		 WHERE data LIKE 'table %'");
};
is($peek->('on_b1', 0), 'app.t: INSERT:501,pub.p: INSERT,app.t: UPDATE:511',
	'a slot made on the first branch: its changes and the shared one');
is($peek->('on_b2', 0), 'app.t: INSERT:502,pub.p: INSERT,app.t: TRUNCATE',
	'a slot made on the second branch: its changes and the shared one');
is($peek->('on_b2', $b1), 'app.t: INSERT:502,pub.p: INSERT,app.t: TRUNCATE',
	'read from a session on the other branch, the same');
is( $peek->('on_none', $b1),
	'app.t: INSERT:501,app.t: INSERT:502,pub.p: INSERT,app.t: UPDATE:511,app.t: TRUNCATE',
	'a slot made on no branch: all of them');
$node->restart;
is($peek->('on_b1', 0), 'app.t: INSERT:501,pub.p: INSERT,app.t: UPDATE:511',
	'a slot keeps its branch across a restart');
$node->safe_psql('postgres', "SELECT 1 FROM pg_copy_logical_replication_slot('on_b1', 'copy_b1')");
is($peek->('copy_b1', 0), 'app.t: INSERT:501,pub.p: INSERT,app.t: UPDATE:511',
	'a copy, made on no branch, keeps the source\'s branch');
$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('copy_b1')");

# a failover slot on a branch is refused: a standby's synced copy would not
# keep the branch
my ($ret, $out, $err) = $node->psql('postgres',
	"SET catalog_branch = $b1; SELECT pg_create_logical_replication_slot('fo', 'test_decoding', false, false, true)");
like($err, qr/cannot enable failover for a replication slot made on a branch/,
	'a failover slot cannot be made on a branch');
($ret, $out, $err) = $node->psql('postgres', "ALTER_REPLICATION_SLOT on_b1 (FAILOVER)",
	replication => 'database');
like($err, qr/cannot enable failover for a replication slot made on a branch/,
	'nor a branch\'s slot made one');

# a transaction on both branches, each table's values toasted: a slot on
# the second branch has the second's whole, the first's left out
for my $b ($b1, $b2)
{
	on($b, 'CREATE TABLE app.big (id int PRIMARY KEY, t text)');
}
$node->safe_psql('postgres', "
	BEGIN;
	SET catalog_branch = $b1;
	INSERT INTO app.big SELECT g, string_agg(md5(g::text || h::text), '') FROM generate_series(1, 20) g, generate_series(1, 200) h GROUP BY g;
	SET catalog_branch = $b2;
	INSERT INTO app.big SELECT 1, string_agg(md5(h::text), '') FROM generate_series(1, 200) h;
	COMMIT;");
is( $node->safe_psql('postgres', "
	SELECT string_agg(substring(data from 'id\\[integer\\]:([0-9]+)') || ':' ||
		length(substring(data from 't\\[text\\]:''([0-9a-f]*)''')), ',')
	FROM pg_logical_slot_get_changes('on_b2', NULL, NULL)
	WHERE data LIKE 'table app.big%'"),
	'1:6400',
	'a toasted value of the branch\'s whole, the other branch\'s left out');
for my $s ('on_b1', 'on_b2', 'on_none')
{
	$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('$s')");
}

# a publication of every table, and a subscriber in another database whose
# slot was made on the second branch: it replicates that branch's tables
# and no other's (its connection on that branch too, so that the tables
# it subscribes to are that branch's)
$node->safe_psql('postgres', 'CREATE DATABASE sub');
$node->safe_psql('sub',
	'CREATE SCHEMA app; CREATE TABLE app.t (id int PRIMARY KEY, v int);
	 CREATE TABLE app.big (id int PRIMARY KEY, t text);
	 CREATE SCHEMA pub; CREATE TABLE pub.p (id int PRIMARY KEY, note text)');
$node->safe_psql('postgres', 'CREATE PUBLICATION pub_all FOR ALL TABLES');
# (a subscription in the same cluster cannot make its own slot)
on($b2, "SELECT 1 FROM pg_create_logical_replication_slot('s2', 'pgoutput')");
my $connstr = $node->connstr('postgres') . " options='-c catalog_branch=$b2'";
$node->safe_psql('sub',
	"CREATE SUBSCRIPTION s2 CONNECTION \$\$$connstr\$\$ PUBLICATION pub_all WITH (create_slot = false, slot_name = 's2', copy_data = false)");
$node->wait_for_subscription_sync($node, 's2', 'sub');
on($b1, 'INSERT INTO app.t VALUES (7001, 1)');
on($b2, 'INSERT INTO app.t VALUES (7002, 2)');
$node->wait_for_catchup('s2');
is($node->safe_psql('sub', 'SELECT string_agg(id::text, \',\' ORDER BY id) FROM app.t'),
	'7002', 'the subscriber has the second branch\'s row, not the first\'s');
$node->safe_psql('sub', 'DROP SUBSCRIPTION s2');

$node->stop;
done_testing();
