# Copyright (c) 2026, PostgreSQL Global Development Group

# A subscription to a branch: its connection is on the branch, so the
# tables it subscribes to, its initial copy of them and the slot it makes
# are the branch's, and its changes are the branch's and the shared
# schemas', never another branch's.  Streaming from a slot made on another
# branch than the connection's is refused.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $pub = ProllyTest::new_node('publisher', allows_streaming => 'logical');
my $sub = ProllyTest::new_node('subscriber');
$pub->start;
$sub->start;

my $b1 = $pub->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b2 = $pub->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on
{
	my ($branch, $sql) = @_;
	return $pub->safe_psql('postgres', "SET catalog_branch = $branch; $sql");
}

for my $b ($b1, $b2)
{
	on($b,
		"CREATE SCHEMA app VERSIONED;
		 CREATE TABLE app.t (id int PRIMARY KEY, v text);
		 INSERT INTO app.t VALUES (1, 'b$b first')");
}
# (a publication's schema membership is the branch's whose schema it is:
# made on b2, for b2's app)
on($b2, 'CREATE PUBLICATION pub_app FOR TABLES IN SCHEMA app');

$sub->safe_psql('postgres', 'CREATE SCHEMA app; CREATE TABLE app.t (id int PRIMARY KEY, v text)');
my $conn = $pub->connstr('postgres') . " options='-c catalog_branch=$b2'";
$sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION s CONNECTION \$\$$conn\$\$ PUBLICATION pub_app");
$sub->wait_for_subscription_sync($pub, 's');
is($sub->safe_psql('postgres', 'SELECT string_agg(v, \',\' ORDER BY id) FROM app.t'),
	"b$b2 first", 'the initial copy is the branch\'s table');

on($b1, "INSERT INTO app.t VALUES (2, 'b$b1 second')");
on($b2, "INSERT INTO app.t VALUES (3, 'b$b2 second')");
$pub->wait_for_catchup('s');
# (the publication names b2's table, so b1's isn't published anyway; that
# decoding leaves another branch's changes out is t/202's)
is($sub->safe_psql('postgres', 'SELECT string_agg(v, \',\' ORDER BY id) FROM app.t'),
	"b$b2 first,b$b2 second", 'and its changes are the branch\'s, not the other branch\'s');

# a connection on another branch than the slot's can't stream from it
$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION s DISABLE');
$pub->poll_query_until('postgres',
	"SELECT NOT active FROM pg_replication_slots WHERE slot_name = 's'");
my $wrong = $pub->connstr('postgres') . " options='-c catalog_branch=$b1'";
$sub->safe_psql('postgres', "ALTER SUBSCRIPTION s CONNECTION \$\$$wrong\$\$");
my $offset = -s $pub->logfile;
$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION s ENABLE');
$pub->wait_for_log(qr/replication slot "s" was made on branch $b2, and this connection is on branch $b1/,
	$offset);
pass('streaming from a slot of another branch is refused');

$sub->safe_psql('postgres', "ALTER SUBSCRIPTION s CONNECTION \$\$$conn\$\$");
$pub->wait_for_catchup('s');
$sub->safe_psql('postgres', 'DROP SUBSCRIPTION s');

$sub->stop;
$pub->stop;
done_testing();
