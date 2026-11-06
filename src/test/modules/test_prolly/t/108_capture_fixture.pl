# Copyright (c) 2026, PostgreSQL Global Development Group

# The check capture is held to: after each step of a fixture touching every kind of value and most
# kinds of table, each table's row map is, to the byte, the one layer 4 makes by reading the table.

use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $fixture = dirname(__FILE__) . '/../capture_fixture';
our $TODO;
my $node = PostgreSQL::Test::Cluster->new('fixture');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $b = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $use = "SET catalog_branch = $b;";
$node->safe_psql('postgres', "$use CREATE SCHEMA rt VERSIONED");
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
$node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'})
  or die 'no worker';

# Tables DDL has left capture wrong about, which each_table_same goes by.
my %known_wrong;

sub caught_up
{
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres',
		qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die "capture did not reach $to";
}

# A table is as layer 4 reads it; one capture has no map of must have no rows.
sub table_same
{
	my ($step, $table) = @_;
	my $map = $node->safe_psql('postgres',
		"$use SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 'rt.$table'::regclass::oid");
	if ($map eq '')
	{
		is($node->safe_psql('postgres', "$use SELECT count(*) FROM ONLY rt.$table"), '0', "$step: $table has no map and no rows");
	}
	else
	{
		is($map, $node->safe_psql('postgres', "$use SELECT test_ws_rows_hash('rt', '$table')"), "$step: $table");
	}
}

sub each_table_same
{
	my ($step) = @_;
	caught_up();
	my @tables = split /\n/, $node->safe_psql('postgres',
		"$use SELECT c.relname FROM pg_class c WHERE c.relnamespace = 'rt'::regnamespace AND c.relkind = 'r' ORDER BY 1");
	table_same($step, $_) for grep { !$known_wrong{$_} } @tables;
	return scalar(@tables);
}

sub run_file
{
	my ($file) = @_;
	$node->safe_psql('postgres', "$use\n" . slurp_file("$fixture/$file"));
}

run_file('load.sql');
ok(each_table_same('load') >= 10, 'the fixture has its tables');
run_file('dml.sql');
each_table_same('changes');

# the same again, with the session's settings other than layer 4's
$node->safe_psql('postgres', qq{$use SET TimeZone = 'Asia/Tokyo'; SET DateStyle = 'German'; SET IntervalStyle = 'sql_standard';
	SET extra_float_digits = -5; SET bytea_output = 'escape'; SET lc_monetary = 'C';
	UPDATE rt.kinds SET tstz = tstz + interval '1 hour', f8 = f8 * 2, by = by, iv = iv + interval '1 day', mo = mo + 1::money WHERE a % 5 = 0});
each_table_same('under other settings');

# DDL that changes a table's rows and tells no change of them, leaving its layout as it was: until
# DDL says what it did, the map stays as it was, and this is what shows it
sub ddl_step
{
	my ($name, $table, $setup, $ddl) = @_;
	$node->safe_psql('postgres', "$use SET search_path = rt; $setup");
	each_table_same("before $name");
	$node->safe_psql('postgres', "$use SET search_path = rt; $ddl");
	caught_up();
	{
		local $TODO = 'DDL says nothing of what it did to a table\'s rows';
		table_same($name, $table);
	}
	$known_wrong{$table} = 1;
	each_table_same("after $name");
}

ddl_step('a column dropped and added again', 'readd',
	'CREATE TABLE readd (id integer PRIMARY KEY, a integer); INSERT INTO readd VALUES (1, 10)',
	'ALTER TABLE readd DROP COLUMN a; ALTER TABLE readd ADD COLUMN a integer; INSERT INTO readd VALUES (2, 20)');
ddl_step('a retype to the same type', 'retype',
	'CREATE TABLE retype (id integer PRIMARY KEY, a integer); INSERT INTO retype VALUES (1, 10)',
	'ALTER TABLE retype ALTER a TYPE integer USING a + 1');
ddl_step('an enum value renamed', 'named',
	q{CREATE TYPE e AS ENUM ('one', 'two'); CREATE TABLE named (id integer PRIMARY KEY, a e); INSERT INTO named VALUES (1, 'one')},
	q{ALTER TYPE e RENAME VALUE 'one' TO 'uno'});

# a table dropped and made again is another table, whose map starts anew
$node->safe_psql('postgres', "$use CREATE TABLE rt.again (id integer PRIMARY KEY); INSERT INTO rt.again VALUES (1)");
each_table_same('before a table is made again');
$node->safe_psql('postgres', "$use DROP TABLE rt.again; CREATE TABLE rt.again (id integer PRIMARY KEY); INSERT INTO rt.again VALUES (2)");
each_table_same('after a table is made again');

unlike(slurp_file($node->logfile), qr/ERROR:.*capture to trees/, 'capture never stopped');
$node->stop;
done_testing();
