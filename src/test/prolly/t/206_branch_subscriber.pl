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

# the subscription's tables are its branch's: their pg_subscription_rel
# rows are seen on that branch alone
my $rel_rows = "SELECT count(*) FROM pg_subscription_rel r JOIN pg_subscription s ON s.oid = r.srsubid WHERE s.subname = 's'";
is(on_sub($s2, $rel_rows), '2', 'the subscription\'s tables, on its branch');
is(on_sub($s1, $rel_rows), '0', 'none on another branch');
is($sub->safe_psql('postgres', $rel_rows), '0', 'nor on no branch');

# pg_dump writes the subscription's branch
my ($dump, $derr);
IPC::Run::run([ 'pg_dump', '--no-sync', '-s', '-d', $sub->connstr('postgres') ],
	'>', \$dump, '2>', \$derr);
like($dump, qr/CREATE SUBSCRIPTION s .*catalog_branch = $s2/, 'pg_dump writes the subscription\'s branch');

# made from a session on a branch, without the option: the session's branch
on_sub($s1, 'CREATE TABLE app.u (id int PRIMARY KEY)');
on_sub($s1, "CREATE SUBSCRIPTION s_on_branch CONNECTION \$\$" . $pub->connstr('postgres') .
	"\$\$ PUBLICATION p WITH (slot_name = s_on_branch)");
$sub->wait_for_subscription_sync($pub, 's_on_branch');
is($sub->safe_psql('postgres', "SELECT subcatalogbranch = $s1 FROM pg_subscription WHERE subname = 's_on_branch'"),
	't', 'a subscription made on a branch is the branch\'s');
is(on_sub($s1, 'SELECT count(*) FROM app.t'), '2', 'and replicates into its tables');
$sub->safe_psql('postgres', 'DROP SUBSCRIPTION s_on_branch');

# a trigger on the subscriber's branch table whose function is a shared
# security definer one runs on no branch, as anywhere: the worker's branch
# doesn't capture it
$sub->safe_psql('postgres', "CREATE TABLE public.seen (b text);
	CREATE FUNCTION public.note() RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER AS
	\$\$BEGIN INSERT INTO public.seen VALUES (current_setting('catalog_branch')); RETURN NEW; END\$\$");
on_sub($s2, "CREATE TRIGGER t_note AFTER INSERT ON app.t FOR EACH ROW EXECUTE FUNCTION public.note();
	ALTER TABLE app.t ENABLE ALWAYS TRIGGER t_note");
$pub->safe_psql('postgres', "INSERT INTO app.t VALUES (4, 'fourth')");
$pub->wait_for_catchup('s');
is($sub->safe_psql('postgres', 'SELECT string_agg(b, \',\') FROM public.seen'), '0',
	'a shared definer trigger function runs on no branch inside the worker');

$sub->safe_psql('postgres', 'DROP SUBSCRIPTION s');
$sub->stop;
$pub->stop;
done_testing();
