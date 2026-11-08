# Copyright (c) 2026, PostgreSQL Global Development Group

# A barrier taken while capture's slot is being made waits for the slot, holding nothing the slot's
# making needs, and then has its root: the slot is made reading every table under the lock a barrier
# holds while it waits for capture, so a barrier holding it then would wait out its time.

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
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch;";

# The worker held as its slot is being made, before it reads the tables.
$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-slot-snapshot', 'wait')});
$node->safe_psql('postgres', "$on CREATE SCHEMA app VERSIONED; CREATE TABLE app.t (id integer PRIMARY KEY); INSERT INTO app.t VALUES (1)");
$node->wait_for_event('capture worker', 'capture-slot-snapshot');

my $barrier = $node->background_psql('postgres', on_error_stop => 0);
$barrier->query_until(qr/started/, "\\echo started\n$on SELECT test_capture_barrier('app', 20000) IS NOT NULL AS got;\n\\echo done\n");
ok($node->poll_query_until('postgres',
	q{SELECT count(*) = 1 FROM pg_stat_activity WHERE query LIKE 'SELECT test_capture_barrier%' AND wait_event = 'CaptureBarrier'}),
	'a barrier waits for the slot');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_locks WHERE locktype = 'object' AND objsubid = 2}), '0',
	'holding nothing its making needs');
$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-snapshot'); SELECT injection_points_wakeup('capture-slot-snapshot')});
my $start = time;
like($barrier->query_until(qr/done/, ''), qr/^t$/m, 'once the slot is made, the barrier has its root');
ok(time - $start < 15, 'well within its time');
like($barrier->{stderr}, qr/^$/, 'with no error');
$barrier->quit;

is($node->safe_psql('postgres', "$on SELECT test_ws_root_capture('app') = test_capture_barrier('app')"), 't',
	'and capture has what the tables hold');

done_testing();
