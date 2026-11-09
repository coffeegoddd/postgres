# Copyright (c) 2026, PostgreSQL Global Development Group

# A barrier taken while capture's slot is being made waits for the slot, holding nothing the slot's
# making needs, and then has its root: the slot is made reading every table under the lock a barrier
# holds while it waits for capture, and once every transaction with an ID has ended, so a barrier
# holding either then would wait out its time.  So for a move, which waits before its transaction
# has an ID, and for a slot lost and made again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes' || $windows_os)
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('barrier_slot');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\nchunk_free_in_background = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";
my $rows_lock = q{SELECT count(*) FROM pg_locks WHERE locktype = 'object' AND classid = 'pg_database'::regclass
	AND objid = (SELECT oid FROM pg_database WHERE datname = 'postgres') AND objsubid = 2};

sub waiting_barrier
{
	my ($like) = @_;
	return $node->poll_query_until('postgres',
		qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE query LIKE '$like' AND wait_event = 'CaptureBarrier'});
}

# The slot lost: the worker, idle, stopped, and more WAL written than the slot may keep, so a
# checkpoint invalidates it once the worker, let go, ends; the next worker is held before it drops
# the slot lost, and again as it makes it anew.
my $filled = 0;
sub lose_slot
{
	my $worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	# (stopped idle, holding nothing: a worker woken in between is let go and stopped again; what it's
	# waiting on is read with no lock taken, in a time, as a stopped worker's status may be half
	# written)
	my $idle = qq{SELECT wait_event = 'CaptureWorkerMain' FROM pg_stat_activity WHERE pid = $worker};
	for (;;)
	{
		$node->poll_query_until('postgres', $idle) or die 'the worker is not idle';
		kill 'STOP', $worker;
		my ($rc, $out) = $node->psql('postgres', "SET statement_timeout = '5s'; $idle");
		last if $rc == 0 && $out eq 't';
		kill 'CONT', $worker;
	}
	$filled++;
	my $log_at = -s $node->logfile;
	$node->safe_psql('postgres', qq{CREATE TABLE public.filler$filled AS SELECT g, repeat('w', 200) AS v
		FROM generate_series(1, 100000) g; SELECT pg_switch_wal()});
	my $checkpoint = $node->background_psql('postgres');
	$checkpoint->query_until(qr/started/, "\\echo started\nCHECKPOINT;\n\\echo checkpointed\n");
	$node->wait_for_log(qr/terminating process $worker to release replication slot/, $log_at);
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-slot-lost', 'wait');
		SELECT injection_points_attach('capture-slot-snapshot', 'wait')});
	kill 'CONT', $worker;
	$checkpoint->query_until(qr/checkpointed/, '');
	$checkpoint->quit;
	$node->wait_for_event('capture worker', 'capture-slot-lost');
}

sub slot_made_again
{
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-lost'); SELECT injection_points_wakeup('capture-slot-lost')});
	$node->wait_for_event('capture worker', 'capture-slot-snapshot');
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-snapshot'); SELECT injection_points_wakeup('capture-slot-snapshot')});
}

# The worker held as its slot is being made, before it reads the tables.
$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-slot-snapshot', 'wait')});
$node->safe_psql('postgres', "$on CREATE SCHEMA app VERSIONED; CREATE TABLE app.t (id integer PRIMARY KEY, v integer);
	INSERT INTO app.t VALUES (1, 1)");
$node->wait_for_event('capture worker', 'capture-slot-snapshot');

my $barrier = $node->background_psql('postgres', on_error_stop => 0);
$barrier->query_until(qr/started/, "\\echo started\n$on SELECT test_capture_barrier('app', 30000) IS NOT NULL AS got;\n\\echo done\n");
ok(waiting_barrier('SELECT test_capture_barrier%'), 'a barrier waits for the slot');
is($node->safe_psql('postgres', $rows_lock), '0', 'holding none of what the slot\'s making needs');
$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-snapshot'); SELECT injection_points_wakeup('capture-slot-snapshot')});
like($barrier->query_until(qr/done/, ''), qr/^t$/m, 'once the slot is made, the barrier has its root');
is($barrier->{stderr}, '', 'with no error');
$barrier->quit;
is($node->safe_psql('postgres', "$on SELECT test_capture_barrier('app')"),
	$node->safe_psql('postgres', "$on SELECT test_ws_root_capture('app')"), 'and capture has what the tables hold');

$node->safe_psql('postgres', q{ALTER SYSTEM SET max_slot_wal_keep_size = '1MB'; SELECT pg_reload_conf()});
lose_slot();
is($node->safe_psql('postgres', q{SELECT invalidation_reason IS NOT NULL FROM pg_replication_slots
	WHERE slot_name LIKE 'prolly\_capture\_%'}), 't', 'the slot is lost, and there');

# A root of the tables as they are, read of them, and a move to it once they've changed, waiting
# for the slot lost while it's there, and as it's made again: before its transaction has an ID.
my $root = $node->safe_psql('postgres', "$on SELECT test_ws_root_capture('app')");
$node->safe_psql('postgres', "$on UPDATE app.t SET v = 2; INSERT INTO app.t VALUES (2, 2)");
my $mover = $node->background_psql('postgres', on_error_stop => 0);
$mover->query_until(qr/started/, "\\echo started\n$on SELECT test_ws_move('app', '$root', 60000) IS NOT NULL AS moved;\n\\echo done\n");
ok(waiting_barrier('SELECT test_ws_move%'), 'a move waits for the slot lost to be made again');
is($node->safe_psql('postgres', $rows_lock), '0', 'holding no lock');
my $no_xid = q{SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'SELECT test_ws_move%' AND backend_xid IS NOT NULL};
is($node->safe_psql('postgres', $no_xid), '0', 'nor a transaction ID');
$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-lost'); SELECT injection_points_wakeup('capture-slot-lost')});
$node->wait_for_event('capture worker', 'capture-slot-snapshot');
is($node->safe_psql('postgres', $no_xid), '0', 'and as it is made again, still with no transaction ID');
$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-snapshot'); SELECT injection_points_wakeup('capture-slot-snapshot')});
like($mover->query_until(qr/done/, ''), qr/^t$/m, 'once it is, the move moves');
is($mover->{stderr}, '', 'with no error');
$mover->quit;
is($node->safe_psql('postgres', "$on SELECT test_ws_root_capture('app')"), $root, 'the tables hold the root');
is($node->safe_psql('postgres', "$on SELECT test_capture_barrier('app')"), $root, 'and capture has it');

# A move that has its transaction ID and its tables when the slot is lost: it can't wait for the
# slot, which waits for it, and it's a serialization failure at once.
$node->safe_psql('postgres', "$on UPDATE app.t SET v = 3");
$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-barrier-locked', 'wait')});
$mover = $node->background_psql('postgres', on_error_stop => 0);
$mover->query_until(qr/started/, "\\echo started\n$on SELECT test_ws_move('app', '$root', 60000) IS NOT NULL AS moved;\n\\echo done\n");
$node->wait_for_event('client backend', 'capture-barrier-locked');
lose_slot();
my $start = time;
$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-barrier-locked'); SELECT injection_points_wakeup('capture-barrier-locked')});
$mover->query_until(qr/done/, '');
like($mover->{stderr}, qr/capture's slot was lost as a commit from capture held its tables/,
	'a move with its transaction ID and its tables, the slot lost, fails to be tried again');
ok(time - $start < 30, 'at once, not waiting out its time');
$mover->quit;
slot_made_again();
$node->safe_psql('postgres', "$on SELECT test_ws_move('app', '$root')");
is($node->safe_psql('postgres', "$on SELECT test_ws_root_capture('app')"), $root, 'tried again, it moves');
is($node->safe_psql('postgres', "$on SELECT test_capture_barrier('app')"), $root, 'and capture has it');

# A branch's commit, the slot lost: it waits with no transaction ID, the root of its schemas put
# together only once it has them, and commits once the slot is made again.
$node->safe_psql('postgres', "$on SELECT test_branch_link('main', $branch)");
$node->safe_psql('postgres', "$on UPDATE app.t SET v = 4");
lose_slot();
my $committer = $node->background_psql('postgres', on_error_stop => 0);
$committer->query_until(qr/started/, "\\echo started\n$on SELECT test_dbroot_schemas(test_branch_commit('main', 'c')) = 'app=' || test_ws_root_capture('app') AS committed;\n\\echo done\n");
ok(waiting_barrier('SELECT test_dbroot_schemas(test_branch_commit%'), 'a branch\'s commit waits for the slot lost to be made again');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'SELECT test_dbroot_schemas(test_branch_commit%'
	AND backend_xid IS NOT NULL}), '0', 'with no transaction ID');
slot_made_again();
like($committer->query_until(qr/done/, ''), qr/^t$/m, 'once it is, the branch commits what its tables hold');
is($committer->{stderr}, '', 'with no error');
$committer->quit;
is($node->safe_psql('postgres', "$on SELECT test_capture_barrier('app')"), $node->safe_psql('postgres', "$on SELECT test_ws_root_capture('app')"),
	'and capture has what its tables hold');

done_testing();
