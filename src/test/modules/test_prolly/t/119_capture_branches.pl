# Copyright (c) 2026, PostgreSQL Global Development Group

# Many branches written at once: each branch's schema of the same name, written by sessions of its
# own in transactions interleaved with every other's, and each branch's tree, after, equal to its
# own tables and no other's.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branches');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\nlogical_decoding_work_mem = 64kB\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $n = 5;
my @on;
for my $i (0 .. $n - 1)
{
	my $b = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
	push @on, "SET catalog_branch = $b;";
	$node->safe_psql('postgres', "$on[$i] CREATE SCHEMA app VERSIONED;
		CREATE TABLE app.t (id integer PRIMARY KEY, branch integer, v text);
		CREATE TABLE app.k (branch integer, v text)");
}
$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker'}) or die 'no worker';
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = current_database()});

sub caught_up
{
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die 'capture did not catch up';
}

# Every branch written at once: a session each, each running transactions of inserts, updates and
# deletes, its statements sent before any's answered, so that the WAL has them all interleaved.
my @s = map { $node->background_psql('postgres', on_error_stop => 1) } 0 .. $n - 1;
$s[$_]->query_safe("$on[$_] SET search_path = app") for 0 .. $n - 1;
for my $round (1 .. 20)
{
	for my $i (0 .. $n - 1)
	{
		my $base = $round * 100;
		$s[$i]->query_safe(
			"BEGIN; INSERT INTO t SELECT $base + g, $i, 'r$round' FROM generate_series(1, 30) g;
			UPDATE t SET v = v || '+' WHERE id % 7 = $round % 7; DELETE FROM t WHERE id % 11 = $round % 11 AND id < $base;
			INSERT INTO k VALUES ($i, 'r$round'), ($i, 'r$round'); COMMIT");
	}
}
$_->quit for @s;
caught_up();

for my $i (0 .. $n - 1)
{
	is($node->safe_psql('postgres', "$on[$i] SELECT count(DISTINCT branch) || ':' || min(branch) FROM app.t"), "1:$i",
		"branch ${i}'s table has only its own rows");
	for my $table ('t', 'k')
	{
		is( $node->safe_psql('postgres',
				"$on[$i] SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 'app.$table'::regclass::oid"),
			$node->safe_psql('postgres', "$on[$i] SELECT test_ws_rows_hash('app', '$table')"),
			"branch $i: $table");
	}
	my $root = $node->safe_psql('postgres', "$on[$i] SELECT test_capture_barrier('app')");
	isnt($root, '', "branch $i: a root");
	is($root, $node->safe_psql('postgres', "$on[$i] SELECT test_ws_root_capture('app')"), "branch $i: its root is its tables");
}

# and each branch's own position
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_history.capture_positions WHERE branch <> 0"), $n, 'a position for each branch');

$node->stop;
done_testing();
