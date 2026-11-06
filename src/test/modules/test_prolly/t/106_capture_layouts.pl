# Copyright (c) 2026, PostgreSQL Global Development Group

# Each change capture decodes goes by its table's layout as the catalog was at that change: the
# same columns, tags and key that layer 4 reads from the catalogs, with no message saying so.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('layouts');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_min_messages = debug1\nlog_line_prefix = ''\nautovacuum = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $b = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $use = "SET catalog_branch = $b;";
$node->safe_psql('postgres', "$use CREATE SCHEMA s VERSIONED");
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});

sub caught_up
{
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres',
		qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die "capture did not reach $to";
}

# Runs a statement, and gives the layouts capture read for the changes it wrote.
sub layouts
{
	my ($sql) = @_;
	caught_up();
	my $at = -s $node->logfile;
	$node->safe_psql('postgres', "$use $sql");
	caught_up();
	return [ slurp_file($node->logfile, $at) =~ /capture layout: (\S+ .*)$/mg ];
}

# What layer 4 reads of a table now.
sub read_now
{
	my ($table) = @_;
	return "$table " . $node->safe_psql('postgres', "$use SELECT test_ws_schema_shape('s', '$table')");
}

# Each of these tables' changes are read as layer 4 reads the table.
my %tables = (
	'plain' => ['id integer PRIMARY KEY, v text', q{1, 'x'}],
	'key_out_of_order' => ['a integer, b integer, c integer, d text, PRIMARY KEY (c, a)', q{1, 2, 3, 'x'}],
	'key_with_include' => ['a integer, b integer, c text, PRIMARY KEY (b) INCLUDE (c)', q{1, 2, 'x'}],
	'key_deferrable' => ['a integer, b integer, PRIMARY KEY (b) DEFERRABLE', '1, 2'],
	'keyless' => ['a integer, b text', q{1, 'x'}],
	'unique_only' => ['a integer UNIQUE NOT NULL, b text', q{1, 'x'}],
	'stored' => ['id integer PRIMARY KEY, a integer, g integer GENERATED ALWAYS AS (a + 1) STORED', '1, 2'],
	'virtual' => ['id integer PRIMARY KEY, a integer, g integer GENERATED ALWAYS AS (a + 1) VIRTUAL', '1, 2'],
);
for my $t (sort keys %tables)
{
	my ($columns, $row) = @{ $tables{$t} };
	$node->safe_psql('postgres', "$use CREATE TABLE s.$t ($columns)");
	is_deeply(layouts("INSERT INTO s.$t VALUES ($row)"), [ read_now($t) ], "$t: as layer 4 reads it");
}

# after a statement, the new layout, the same for a change that doesn't touch the rows' tags
$node->safe_psql('postgres', "$use CREATE TABLE s.t (id integer PRIMARY KEY, a integer, b integer)");
my $before = read_now('t');
is_deeply(layouts('INSERT INTO s.t VALUES (1, 2, 3)'), [$before], 'a table to change');
$node->safe_psql('postgres', "$use ALTER TABLE s.t RENAME COLUMN a TO renamed");
is_deeply(layouts('INSERT INTO s.t VALUES (2, 2, 3)'), [$before], 'a rename keeps the tags');
is(read_now('t'), $before, 'as layer 4 reads it too');
$node->safe_psql('postgres', "$use ALTER TABLE s.t DROP COLUMN b");
my $dropped = read_now('t');
isnt($dropped, $before, 'a dropped column is gone from the layout');
is_deeply(layouts('INSERT INTO s.t VALUES (3, 2)'), [$dropped], 'and from what decoding reads');
$node->safe_psql('postgres', "$use ALTER TABLE s.t ADD COLUMN c text");
is_deeply(layouts(q{INSERT INTO s.t VALUES (4, 2, 'c')}), [ read_now('t') ], 'an added column');
$node->safe_psql('postgres', "$use ALTER TABLE s.t ALTER COLUMN c TYPE varchar(10)");
is_deeply(layouts(q{INSERT INTO s.t VALUES (5, 2, 'c')}), [ read_now('t') ], 'a retyped column');

# in one transaction, each change has the layout it was written under
$node->safe_psql('postgres', "$use CREATE TABLE s.x (id integer PRIMARY KEY, a integer)");
my $old = read_now('x');
my $got = layouts('BEGIN; INSERT INTO s.x VALUES (1, 1); ALTER TABLE s.x ADD COLUMN b integer; INSERT INTO s.x VALUES (2, 2, 2); COMMIT');
my $new = read_now('x');
isnt($new, $old, 'the transaction changed the layout');
is_deeply($got, [ $old, $new ], 'a change before DDL in its transaction has the old layout, one after it the new');

$got = layouts('BEGIN; INSERT INTO s.x VALUES (3, 3, 3); ALTER TABLE s.x DROP CONSTRAINT x_pkey; INSERT INTO s.x VALUES (4, 4, 4); COMMIT');
my $keyless = read_now('x');
like($keyless, qr/nkey=0 /, 'the key is gone');
is_deeply($got, [ $new, $keyless ], 'a key dropped between two changes');

# committed before DDL but decoded after it, a change still has the layout it was written under
SKIP:
{
	skip 'no way to stop a process', 2 if $windows_os;
	$node->safe_psql('postgres', "$use CREATE TABLE s.late (id integer PRIMARY KEY, a integer)");
	caught_up();
	my $worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	my $at = -s $node->logfile;
	kill 'STOP', $worker;
	$node->safe_psql('postgres', "$use INSERT INTO s.late VALUES (1, 1)");
	$old = read_now('late');
	$node->safe_psql('postgres', "$use ALTER TABLE s.late ADD COLUMN b integer");
	$node->safe_psql('postgres', "$use INSERT INTO s.late VALUES (2, 2, 2)");
	$new = read_now('late');
	isnt($new, $old, 'the layout changed while the worker was stopped');
	kill 'CONT', $worker;
	caught_up();
	is_deeply([ slurp_file($node->logfile, $at) =~ /capture layout: (\S+ .*)$/mg ],
		[ $old, $new ], 'each change has its own layout, however late it is decoded');
}

$node->stop;
done_testing();
