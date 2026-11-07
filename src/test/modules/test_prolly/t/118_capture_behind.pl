# Copyright (c) 2026, PostgreSQL Global Development Group

# Capture far behind: a warning once its slot holds past half of max_slot_wal_keep_size, while
# something can still be done; past all of it, the slot invalidated at a checkpoint, and every
# table read again on a new one, so that what was lost is in the maps all the same.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($windows_os)
{
	plan skip_all => 'no way to stop a process';
}

my $node = PostgreSQL::Test::Cluster->new('behind');
$node->init(allows_streaming => 'logical', extra => ['--wal-segsize=1']);
$node->append_conf('postgresql.conf', "log_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\n"
	  . "min_wal_size = 2MB\nmax_wal_size = 4MB\nmax_slot_wal_keep_size = 4MB\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch;";
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0}) or die 'no worker';
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = current_database()});

sub worker
{
	return $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
}

sub run
{
	return $node->safe_psql('postgres', "$on SET search_path = s, public; $_[0]");
}

sub caught_up
{
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die 'capture did not catch up';
}

sub same_map
{
	my ($table, $what) = @_;
	caught_up();
	is( run(qq{SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.$table'::regclass::oid}),
		run(qq{SELECT test_ws_rows_hash('s', '$table')}), $what);
}

run(q{CREATE TABLE t (id integer PRIMARY KEY, v text); INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 100) g});
same_map('t', 'a table');

# held past half of what it may: a warning, once
my $worker = worker();
my $log = -s $node->logfile;
kill 'STOP', $worker;
$node->advance_wal(3);
kill 'CONT', $worker;
ok($node->wait_for_log(qr/WARNING:  capture is behind: its replication slot holds \d+ MB of WAL, past half of "max_slot_wal_keep_size" \(4 MB\)/, $log),
	'capture far behind is warned of');
caught_up();
is($node->safe_psql('postgres', "SELECT wal_status <> 'lost' FROM pg_replication_slots WHERE slot_name = '$slot'"), 't', 'and its slot kept');

# past all of it: the slot invalidated at a checkpoint, and made again, every table read again
$log = -s $node->logfile;
kill 'STOP', $worker;
run(q{UPDATE t SET v = 'lost' WHERE id % 2 = 0; INSERT INTO t VALUES (1000, 'while lost')});
$node->advance_wal(8);
my $cp = $node->background_psql('postgres');
$cp->query_until(qr/started/, "\\echo started\nCHECKPOINT;\n\\echo done\n");
kill 'CONT', $worker;
$cp->{stdout} = '';
$cp->{run}->pump until $cp->{stdout} =~ /done/ || !$cp->{run}->pumpable;
$cp->quit;
ok($node->wait_for_log(qr/invalidating obsolete replication slot "$slot"/, $log), 'its slot invalidated');
ok($node->wait_for_log(qr/WARNING:  capture's replication slot "$slot" was invalidated, and is being made again/, $log),
	'and made again');
$node->poll_query_until('postgres', qq{SELECT wal_status <> 'lost' FROM pg_replication_slots WHERE slot_name = '$slot'})
  or die 'no new slot';
same_map('t', 'what was written meanwhile is in the map all the same');
run(q{DELETE FROM t WHERE id = 1000});
same_map('t', 'and capture goes on');

$node->stop;
done_testing();
