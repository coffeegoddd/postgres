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
my $node = PostgreSQL::Test::Cluster->new('fixture');
$node->init(allows_streaming => 'logical');

# (everything here, the worker included, starts on other settings than those values are read under)
$node->append_conf('postgresql.conf', qq{log_line_prefix = ''
autovacuum = off
capture_to_trees = on
TimeZone = 'Asia/Tokyo'
DateStyle = 'German'
IntervalStyle = 'sql_standard'
extra_float_digits = -5
bytea_output = 'escape'
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $b = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $use = "SET catalog_branch = $b;";
$node->safe_psql('postgres', "$use CREATE SCHEMA rt VERSIONED");
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
$node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'})
  or die 'no worker';

sub caught_up
{
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres',
		qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die "capture did not reach $to";
}

# (the reader on settings other than the worker's, so that a setting neither fixes shows too)
my $reader = q{SET TimeZone = 'America/New_York'; SET DateStyle = 'SQL, DMY'; SET IntervalStyle = 'iso_8601';
	SET extra_float_digits = 0; SET bytea_output = 'hex';};

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
		is($map, $node->safe_psql('postgres', "$use $reader SELECT test_ws_rows_hash('rt', '$table')"), "$step: $table");
	}
}

sub each_table_same
{
	my ($step) = @_;
	caught_up();
	my @tables = split /\n/, $node->safe_psql('postgres',
		"$use SELECT c.relname FROM pg_class c WHERE c.relnamespace = 'rt'::regnamespace AND c.relkind = 'r' ORDER BY 1");
	table_same($step, $_) for @tables;
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

# DDL that changes a table's rows and tells no change of them, leaving its layout as it was: until
# DDL says what it did, the map stays as it was, and this says so, so that it fails when that's
# fixed.  Writing every row again then puts the map right.
sub ddl_step
{
	my ($name, $table, $setup, $ddl) = @_;
	$node->safe_psql('postgres', "$use SET search_path = rt; $setup");
	each_table_same("before $name");
	$node->safe_psql('postgres', "$use SET search_path = rt; $ddl");
	caught_up();
	isnt($node->safe_psql('postgres',
			"$use SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 'rt.$table'::regclass::oid"),
		$node->safe_psql('postgres', "$use $reader SELECT test_ws_rows_hash('rt', '$table')"),
		"$name: its map has not followed");
	$node->safe_psql('postgres', "$use UPDATE rt.$table SET a = a");
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

unlike(slurp_file($node->logfile), qr/^(ERROR|FATAL|PANIC):|terminated by signal/m, 'nothing here went wrong, capture\'s worker included');
$node->stop;
done_testing();
