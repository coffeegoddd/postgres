# Copyright (c) 2026, PostgreSQL Global Development Group

# A subscriber replicating into its own versioned schema: a subscription
# made WITH (catalog_branch = N) runs its workers on its branch N, so the
# tables it copies into and applies changes to are that branch's, and no
# other branch's of the subscriber.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $pub = ProllyTest::new_node('publisher', allows_streaming => 'logical');
my $sub = ProllyTest::new_node('subscriber');
$pub->start;
$sub->start;

$pub->safe_psql('postgres',
	"CREATE SCHEMA app; CREATE TABLE app.t (id int PRIMARY KEY, v text);
	 INSERT INTO app.t VALUES (1, 'first');
	 CREATE PUBLICATION p FOR TABLE app.t");

my $s1 = $sub->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $s2 = $sub->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
for my $b ($s1, $s2)
{
	$sub->safe_psql('postgres',
		"SET catalog_branch = $b; CREATE SCHEMA app VERSIONED;
		 CREATE TABLE app.t (id int PRIMARY KEY, v text)");
}
sub on_sub
{
	my ($branch, $sql) = @_;
	return $sub->safe_psql('postgres', "SET catalog_branch = $branch; $sql");
}

# (no such branch)
my ($ret, $out, $err) = $sub->psql('postgres',
	"CREATE SUBSCRIPTION bad CONNECTION \$\$" . $pub->connstr('postgres') .
	"\$\$ PUBLICATION p WITH (catalog_branch = 2000000000, connect = false)");
like($err, qr/branch 2000000000 has not been allocated/, 'an unallocated branch is refused');

$sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION s CONNECTION \$\$" . $pub->connstr('postgres') .
	"\$\$ PUBLICATION p WITH (catalog_branch = $s2)");
$sub->wait_for_subscription_sync($pub, 's');
is(on_sub($s2, 'SELECT string_agg(v, \',\' ORDER BY id) FROM app.t'), 'first',
	'the initial copy goes into the subscription\'s branch\'s table');
$pub->safe_psql('postgres', "INSERT INTO app.t VALUES (2, 'second')");
$pub->wait_for_catchup('s');
is(on_sub($s2, 'SELECT string_agg(v, \',\' ORDER BY id) FROM app.t'), 'first,second',
	'and its changes');
is(on_sub($s1, 'SELECT count(*) FROM app.t'), '0', 'the other branch\'s table is left alone');
is($sub->safe_psql('postgres', "SELECT subcatalogbranch = $s2 FROM pg_subscription WHERE subname = 's'"),
	't', 'pg_subscription says the branch');

# the branch is the subscription's for good: the tables it subscribes to
# are that branch's
($ret, $out, $err) = $sub->psql('postgres', "ALTER SUBSCRIPTION s SET (catalog_branch = $s1)");
like($err, qr/cannot change the branch of subscription "s"/, 'its branch cannot change');

# a table added to the publication, refreshed, is found on the branch too
$pub->safe_psql('postgres',
	"CREATE TABLE app.u (id int PRIMARY KEY); INSERT INTO app.u VALUES (1);
	 ALTER PUBLICATION p ADD TABLE app.u");
on_sub($s2, 'CREATE TABLE app.u (id int PRIMARY KEY)');
$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION s REFRESH PUBLICATION');
$sub->wait_for_subscription_sync($pub, 's');
is(on_sub($s2, 'SELECT count(*) FROM app.u'), '1', 'a refresh finds the branch\'s new table');

$sub->safe_psql('postgres', 'DROP SUBSCRIPTION s');
$sub->stop;
$pub->stop;
done_testing();
