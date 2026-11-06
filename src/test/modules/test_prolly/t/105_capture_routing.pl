# Copyright (c) 2026, PostgreSQL Global Development Group

# What capture tells its sink goes by the branch of the table's schema: a change on a versioned
# schema comes with that branch, and one on a shared schema isn't told.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('routing');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_min_messages = debug1\nlog_line_prefix = ''\nautovacuum = off\n");
$node->start;
my $b1 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b2 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b3 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
for my $b ($b1, $b2)
{
	$node->safe_psql('postgres', "SET catalog_branch = $b; CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (id integer PRIMARY KEY, v text)");
}
$node->safe_psql('postgres', 'CREATE SCHEMA pub; CREATE TABLE pub.u (id integer PRIMARY KEY, v text)');

sub slot_of
{
	my ($db) = @_;
	return 'prolly_capture_' . $node->safe_psql('postgres', qq{SELECT oid FROM pg_database WHERE datname = '$db'});
}

# Waits until capture in a database has decoded all that's flushed.
sub caught_up
{
	my ($db) = @_;
	my $slot = slot_of($db);
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres',
		qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die "capture of $db did not reach $to";
}

# Runs a statement, waits until capture has decoded what it wrote, and gives what it was told.
sub told
{
	my ($sql, $db) = @_;
	$db //= 'postgres';
	caught_up($db);
	my $at = -s $node->logfile;
	$node->safe_psql($db, $sql);
	caught_up($db);
	my $log = slurp_file($node->logfile, $at);
	return join("\n", $log =~ /capture: (.*)$/mg);
}

is(told("SET catalog_branch = $b1; INSERT INTO s.t VALUES (1, 'one')"),
	"begin\nbranch $b1: insert t old=no new=yes\ncommit", 'a change is told with its branch');
is(told("SET catalog_branch = $b2; INSERT INTO s.t VALUES (1, 'two')"),
	"begin\nbranch $b2: insert t old=no new=yes\ncommit", 'the same names on another branch are that branch\'s');
is(told("SET catalog_branch = $b2; UPDATE s.t SET id = 2"),
	"begin\nbranch $b2: update t old=yes new=yes\ncommit", 'an update');
is(told("SET catalog_branch = $b2; DELETE FROM s.t"),
	"begin\nbranch $b2: delete t old=yes new=no\ncommit", 'a delete');
is(told("SET catalog_branch = $b1; TRUNCATE s.t"),
	"begin\nbranch $b1: truncate t restart=no\ncommit", 'a truncate');
is( told(   "BEGIN; SET LOCAL catalog_branch = $b2; INSERT INTO s.t VALUES (3, 'two');"
		  . "SET LOCAL catalog_branch = $b1; INSERT INTO s.t VALUES (3, 'one'); COMMIT"),
	"begin\nbranch $b2: insert t old=no new=yes\nbranch $b1: insert t old=no new=yes\ncommit",
	'a transaction on two branches has each change on its own');

# what's on a shared schema isn't told, and neither is that of a branch it's written beside
is(told("INSERT INTO pub.u VALUES (1, 'a')"), "begin\ncommit", 'an insert on a shared schema');
is(told("UPDATE pub.u SET id = 2"), "begin\ncommit", 'an update on a shared schema');
is(told("DELETE FROM pub.u"), "begin\ncommit", 'a delete on a shared schema');
is(told("TRUNCATE pub.u"), "begin\ncommit", 'a truncate on a shared schema');
is( told("SET catalog_branch = $b1; BEGIN; INSERT INTO pub.u VALUES (3, 'a'); INSERT INTO s.t VALUES (4, 'a'); COMMIT"),
	"begin\nbranch $b1: insert t old=no new=yes\ncommit", 'from a session on a branch, and beside a branch\'s change');
is( told("SET catalog_branch = $b1; TRUNCATE pub.u, s.t RESTART IDENTITY"),
	"begin\nbranch $b1: truncate t restart=yes\ncommit", 'a truncate naming both tells the branch\'s alone');

# a value kept out of line is the table's change, on the table's branch
is( told("SET catalog_branch = $b1; ALTER TABLE s.t ALTER v SET STORAGE EXTERNAL; INSERT INTO s.t VALUES (5, repeat('x', 10000))"),
	"begin\ncommit\nbegin\nbranch $b1: insert t old=no new=yes\ncommit", 'a TOASTed value is one change on the branch');

# a table of a schema that has since gone, is told as it was
is( told(   "SET catalog_branch = $b1; BEGIN; CREATE SCHEMA gone VERSIONED; CREATE TABLE gone.x (a integer) ;"
		  . 'INSERT INTO gone.x VALUES (1); DROP SCHEMA gone CASCADE; COMMIT'),
	"begin\nbranch $b1: insert x old=no new=yes\ncommit", 'a schema dropped before the commit');

# a copy of a branch is a branch of its own
$node->safe_psql('postgres', "SELECT pg_branch_catalog_copy($b1, $b3)");
is(told("SET catalog_branch = $b3; INSERT INTO s.t VALUES (6, 'three')"),
	"begin\nbranch $b3: insert t old=no new=yes\ncommit", 'a copy\'s changes are the copy\'s');

# a database's settings don't put capture on a branch, which would leave out the others
$node->safe_psql('postgres', 'CREATE DATABASE other');
my $o1 = $node->safe_psql('other', 'SELECT pg_branch_id_allocate()');
my $o2 = $node->safe_psql('other', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('postgres', "ALTER DATABASE other SET catalog_branch = $o1");
$node->safe_psql('other', 'CREATE SCHEMA s VERSIONED');
my $other = slot_of('other');
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$other'}), 'its worker runs');
is($node->safe_psql('postgres', qq{SELECT branch FROM pg_replication_slots WHERE slot_name = '$other'}), '0', 'its slot is on no branch');
$node->safe_psql('other', "SET catalog_branch = $o2; CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (a integer)");
is(told("SET catalog_branch = $o2; INSERT INTO s.t VALUES (1)", 'other'),
	"begin\nbranch $o2: insert t old=no new=yes\ncommit", 'and it tells a branch other than the one the database is set to');

$node->stop;
done_testing();
