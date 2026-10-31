# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum maintains block-mapped storage in the background.  A dropped
# table's queued storage is let go of by the workers after they process the
# database's tables, at most block_map_release_max_per_pass entries a pass,
# so a queue larger than that takes passes, with no extension or VACUUM of a
# block-mapped relation to drain it.  And a page store's sweep, due once
# after the server starts, runs in the background too: it frees the blocks a
# crash leaked (a prepared transaction's drop, committed but stopped before
# it queued the table's storage).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('bmap_release_autovacuum');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
autovacuum_naptime = 1
block_map_release_max_per_pass = 300
max_prepared_transactions = 10
log_min_messages = debug1
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub autovacuum
{
	my ($on) = @_;
	$node->append_conf('postgresql.conf', 'autovacuum = ' . ($on ? 'on' : 'off'));
	$node->reload;
}

# what the workers' passes over database "postgres" reported, from 'offset'
sub passes
{
	my ($offset) = @_;
	my $log = slurp_file($node->logfile, $offset);
	return ($log =~
		  /automatic storage maintenance of database "postgres": (\d+) release entries let go of, (\d+) blocks freed; (\d+) stores swept, (\d+) blocks freed/g
	);
}

sql(q{CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int, b int8) WITH (fillfactor = 10, autovacuum_enabled = off);
	INSERT INTO t SELECT g, g FROM generate_series(1, 20000) g});
my $blocks = sql(q{SELECT pg_relation_size('t') / 8192});
cmp_ok($blocks, '>', 1000, 'the table has more blocks than a pass lets go of');

# The sweep due after a start, run now: a background one would drain the
# queue in full, whatever the bound.
sql(q{SELECT * FROM test_bmap_sweep()});
is(sql(q{SELECT test_bmap_sweep_due()}), 'f', 'no sweep is due');
my $free = sql(q{SELECT test_bmap_free_blocks()});
sql('DROP TABLE t');
cmp_ok(sql(q{SELECT test_bmap_release_pending()}), '>', 0, 'the drop queues');

# Autovacuum on: its workers drain the queue, though no block-mapped
# relation is left to extend or vacuum.
my $offset = -s $node->logfile;
autovacuum(1);
ok( $node->poll_query_until(
		'postgres', q{SELECT test_bmap_release_pending() = 0}),
	'autovacuum drains the queue in the background');
cmp_ok(sql(q{SELECT test_bmap_free_blocks()}) - $free,
	'>=', $blocks, 'and every block of the table is freed');
autovacuum(0);

# In passes of at most block_map_release_max_per_pass entries (and one step
# past it: a step pops up to 128), with no sweep.
my @passes = passes($offset);
my @drained;
my $swept = 0;
while (my ($entries, undef, $stores) = splice(@passes, 0, 4))
{
	push @drained, $entries if $entries > 0;
	$swept += $stores;
}
my $total = 0;
$total += $_ for @drained;
cmp_ok($total, '>', 300 + 128,
	'the drain lets go of more entries than a pass may (a node\'s children are queued as it is let go of)');
cmp_ok(scalar(@drained), '>=', 2, 'over more than one pass');
ok(!grep({ $_ > 300 + 128 } @drained),
	'each letting go of no more than the bound and one step past it')
  or diag("passes: @drained");
is($swept, 0, 'and no sweep, none being due');

# The maintenance holds no snapshot, so no xmin holds back every backend's
# horizon while a worker drains under the cost-based delay (unlike a table's
# vacuum, it doesn't run as a vacuum, whose xmin others' horizons pass
# over).  A slow drain: each step's buffer accesses cost a sleep.  The
# catalogs vacuumed first, so a worker has little else to do.
sql('VACUUM');
sql(q{CREATE TABLE x (a int, b int8) WITH (fillfactor = 10, autovacuum_enabled = off);
	INSERT INTO x SELECT g, g FROM generate_series(1, 20000) g});
sql('DROP TABLE x');
cmp_ok(sql(q{SELECT test_bmap_release_pending()}), '>', 0, 'a drop queues');
$node->append_conf(
	'postgresql.conf', q{
autovacuum_vacuum_cost_delay = 100
autovacuum_vacuum_cost_limit = 1
});
autovacuum(1);
my $draining = q{FROM pg_stat_activity a
	WHERE backend_type = 'autovacuum worker' AND datname = 'postgres'
	  AND wait_event = 'VacuumDelay'
	  AND NOT EXISTS (SELECT FROM pg_stat_progress_vacuum v WHERE v.pid = a.pid)
	  AND NOT EXISTS (SELECT FROM pg_stat_progress_analyze v WHERE v.pid = a.pid)};
ok( $node->poll_query_until(
		'postgres', "SELECT count(*) > 0 $draining"),
	'a worker drains under the cost-based delay');
is( sql("SELECT string_agg(coalesce(backend_xmin::text, 'none'), ',') $draining"),
	'none', 'and it holds no xmin meanwhile');
$node->append_conf('postgresql.conf', 'autovacuum_vacuum_cost_delay = 0');
$node->reload;
ok( $node->poll_query_until(
		'postgres', q{SELECT test_bmap_release_pending() = 0}),
	'the queue drains');
autovacuum(0);

# A crash leaks a table's blocks: its drop, prepared, commits, and the crash
# comes before the drop's storage is queued.  Recovery replays the drop,
# which leaves letting go of the table's storage to the primary's records.
SKIP:
{
	skip 'Injection points not supported by this build', 5
	  if $ENV{enable_injection_points} ne 'yes';

	sql(q{CREATE EXTENSION injection_points;
		CREATE TABLE l (a int, b int8) WITH (fillfactor = 10, autovacuum_enabled = off);
		INSERT INTO l SELECT g, g FROM generate_series(1, 5000) g});
	my $lblocks = sql(q{SELECT pg_relation_size('l') / 8192});
	sql(q{BEGIN; DROP TABLE l; PREPARE TRANSACTION 'p'});
	sql('CHECKPOINT');

	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-predrop', 'wait')});
	$s->query_until(qr/running/, q{
		\echo running
		COMMIT PREPARED 'p';
	});
	$node->wait_for_event('client backend', 'bmap-predrop');
	is(sql(q{SELECT test_bmap_release_pending()}),
		'0', 'the committed drop has queued nothing yet');

	# The crash, then autovacuum on as the server starts: the sweep due
	# after a start runs in the background.
	$node->stop('immediate');
	eval { $s->quit };
	$node->append_conf('postgresql.conf', 'autovacuum = on');
	$offset = -s $node->logfile;
	$node->start;
	is(sql(q{SELECT count(*) FROM pg_class WHERE relname = 'l'}),
		'0', 'the drop committed');
	ok( $node->poll_query_until(
			'postgres', q{SELECT NOT test_bmap_sweep_due()}),
		'autovacuum runs the sweep that is due');
	$node->append_conf('postgresql.conf', 'autovacuum = off');
	$node->reload;

	my $sweep_freed = 0;
	@passes = passes($offset);
	while (my (undef, undef, $stores, $freed) = splice(@passes, 0, 4))
	{
		$sweep_freed += $freed if $stores > 0;
	}
	cmp_ok($sweep_freed, '>=', $lblocks,
		'and it frees the blocks the crash leaked');
	is(sql(q{SELECT freed FROM test_bmap_sweep()}),
		'0', 'nothing is left leaked');
}

# With block_map_release_max_per_pass 0, autovacuum leaves block-mapped
# storage alone, sweeps included: the sweep due after a start stays due
# through a worker's pass over the database.
$node->append_conf('postgresql.conf', 'block_map_release_max_per_pass = 0');
$node->restart;
is(sql(q{SELECT test_bmap_sweep_due()}), 't', 'a sweep is due after the start');
my $zero_offset = -s $node->logfile;
autovacuum(1);
ok(wait_for_pass($zero_offset), 'a worker passes over the database');
is(sql(q{SELECT test_bmap_sweep_due()}), 't',
	'and, with block_map_release_max_per_pass 0, leaves the sweep due');
autovacuum(0);

$node->stop;
done_testing();

# Waits for a worker's storage maintenance pass over database "postgres"
# after 'offset' in the log.
sub wait_for_pass
{
	my ($offset) = @_;
	for (1 .. 300)
	{
		return 1 if scalar(passes($offset)) > 0;
		sleep 1;
	}
	return 0;
}
