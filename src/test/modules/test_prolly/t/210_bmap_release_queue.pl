# Copyright (c) 2026, PostgreSQL Global Development Group

# Dropped storage let go of in the background.  A drop queues its map's
# subtrees on the page store's release queue: no reference count set and
# no block freed, however large the relation.  A drain lets go of them a
# step (one record) at a time.  The queue holds across a crash before, part
# way through and after a drain, and a standby replays it; drained, every
# block is freed once and none leaks (a sweep after finds nothing to free).
# A step that fails leaves the queue as it was.  A dropped copy keeps the
# blocks it shared counted until its entries are drained.  A sweep counts
# what the queue holds: one running while a drop queues blocks frees none
# of them.  A database dropped with a queue not drained goes with it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('bmap_release_queue');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
wal_keep_size = '1GB'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('bmap_release_queue_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub catch_up
{
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
}

my $free = q{SELECT test_bmap_free_blocks()};
my $pending = q{SELECT test_bmap_release_pending()};

# a table of some thousands of blocks, under two subtrees (with no TOAST
# table, whose storage would be dropped with it)
sub make_table
{
	my ($name, $rows) = @_;
	sql(qq{CREATE TABLE $name (a int, b int8) WITH (fillfactor = 10);
		INSERT INTO $name SELECT g, g FROM generate_series(1, $rows) g});
	my $rel = sql(qq{SELECT pg_relation_filenode('$name')});
	# (the blocks it maps, past its length too)
	my $blocks = sql(qq{SELECT pg_relation_size('$name') / 8192 +
		(SELECT count(*) FROM test_bmap_past_end($rel))});
	my $nodes = sql(qq{SELECT count(DISTINCT block) FROM test_bmap_tree($rel)});
	return ($rel, $blocks, $nodes);
}

my ($rel, $blocks, $nodes) = make_table('big', 60000);
cmp_ok($blocks, '>', 2000, 'the table has thousands of blocks');
is(sql(qq{SELECT count(*) FROM test_bmap_tree($rel) WHERE level = 1}), '1',
	'under a subtree of two levels');
sql('CHECKPOINT');
is(sql($pending), '0', 'the queue is empty');
my $free_before = sql($free);

# The drop does no work of the table's size: its subtrees queued.
my $counted = $node->safe_psql('postgres', q{
	SELECT pg_stat_reset_shared('prolly');
	DROP TABLE big;
	SELECT pg_stat_force_next_flush();
	SELECT string_agg(name || '=' || value, ' ' ORDER BY name) FROM pg_stat_prolly
		WHERE name IN ('bmap_refcount_updates', 'bmap_blocks_freed',
			'bmap_release_queued');
});
like($counted, qr/bmap_blocks_freed=0 bmap_refcount_updates=0 bmap_release_queued=[1-4]$/,
	'a drop sets no count and frees no block, and queues a few entries');
is(sql($free), $free_before, 'nothing is freed by the drop');
my $queued = sql($pending);
cmp_ok($queued, '<=', 4, 'the queue holds the subtrees, not the blocks');

# Part of a drain, then a crash: what it let go of stays so, the rest
# stays queued.
cmp_ok(sql(q{SELECT entries FROM test_bmap_release_drain(200)}), '>=', 200,
	'a drain of some steps');
my $mid_pending = sql($pending);
my $mid_free = sql($free);
cmp_ok($mid_free, '>', $free_before, 'part way, some blocks are freed');
cmp_ok($mid_pending, '>', 0, 'and some still queued');
catch_up();
is($standby->safe_psql('postgres', $pending), $mid_pending,
	'the standby replays the queue');
is($standby->safe_psql('postgres', $free), $mid_free,
	'and the drain\'s frees');
$node->stop('immediate');
$node->start;
is(sql($pending), $mid_pending, 'after a crash the queue is as it was');
is(sql($free), $mid_free, 'and so are the free blocks');

# A step that fails before its record leaves the queue as it was.
my ($ret, $out, $err) = $node->psql('postgres', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-release-step', 'error');
	SELECT * FROM test_bmap_release_drain();
});
like($err, qr/bmap-release-step/, 'a step fails');
is(sql($pending), $mid_pending, 'and the queue is as it was');
is(sql($free), $mid_free, 'and the free blocks');

# Drained: every block of the table and its map freed once, nothing leaked.
sql(q{SELECT * FROM test_bmap_release_drain()});
is(sql($pending), '0', 'the queue drains to empty');
is(sql($free) - $free_before, $blocks + $nodes,
	'every block and node of the table is freed');
is(sql(q{SELECT freed FROM prolly_page_sweep()}), '0',
	'and a sweep finds nothing left held by nothing');
my $drained_free = sql($free);
$node->stop('immediate');
$node->start;
is(sql($free), $drained_free, 'a crash after the drain frees nothing more');
is(sql($pending), '0', 'and queues nothing');
catch_up();
is($standby->safe_psql('postgres', $free), sql($free),
	'the standby\'s allocator is the primary\'s');

# A crash before any drain: the queue holds, and drains after.
my ($rel2, $blocks2, $nodes2) = make_table('t2', 24000);
sql('CHECKPOINT');
my $free2 = sql($free);
sql('DROP TABLE t2');
# (the queue's records flushed, by a commit after them)
sql('CREATE TABLE flushed ()');
$node->stop('immediate');
$node->start;
cmp_ok(sql($pending), '>', 0, 'a crash before the drain keeps the queue');
sql(q{SELECT * FROM test_bmap_release_drain()});
is(sql($free) - $free2, $blocks2 + $nodes2, 'which frees the table\'s blocks after it');
is(sql(q{SELECT freed FROM prolly_page_sweep()}), '0', 'none leaked');

# A dropped copy holds what it shared until drained: the source's pages
# stay shared, so a change copies them; drained, they are its own.
my ($rel3) = make_table('t3', 12000);
my $copy = sql(qq{SELECT test_bmap_share($rel3)});
my $digest = sql(qq{SELECT test_bmap_rows_digest($rel3)});
sql(qq{SELECT test_bmap_unlink($copy, false)});
cmp_ok(sql($pending), '>', 0, 'the copy\'s subtrees are queued');
is(sql(qq{SELECT bool_and(refs = 1) FROM test_bmap_map($rel3)}), 't',
	'and still hold the source\'s blocks');
is(sql(qq{SELECT test_bmap_rows_digest($rel3)}), $digest, 'which read the same');
my $free3 = sql($free);
sql(q{SELECT * FROM test_bmap_release_drain()});
is(sql(qq{SELECT bool_and(refs = 0) FROM test_bmap_map($rel3)}), 't',
	'drained, the source holds its blocks alone');
is(sql($free), $free3, 'and none of them is freed');
is(sql(qq{SELECT test_bmap_rows_digest($rel3)}), $digest, 'the source reads the same');

# A sweep counts the queue: a drop that queues while the sweep is under
# way (after its drain) has its blocks counted as held, not freed.
my ($rel4, $blocks4, $nodes4) = make_table('t4', 12000);
sql('CHECKPOINT');
my $free4 = sql($free);
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-sweep-drained', 'wait')});
$s->query_until(qr/started/, qq{
	\\echo started
	SELECT 'freed=' || freed FROM prolly_page_sweep();
	\\echo done
});
$node->wait_for_event('client backend', 'bmap-sweep-drained');
sql('DROP TABLE t4');
sql(q{SELECT injection_points_wakeup('bmap-sweep-drained')});
like($s->query_until(qr/done/, ''), qr/freed=0/,
	'a sweep frees none of what the queue holds');
$s->quit;
cmp_ok(sql($pending), '>', 0, 'which the queue still holds');
sql(q{SELECT * FROM test_bmap_release_drain()});
is(sql($free) - $free4, $blocks4 + $nodes4, 'and the drain frees, once');
is(sql(q{SELECT freed FROM prolly_page_sweep()}), '0', 'none leaked');

# A database dropped with its queue not drained.
sql('CREATE DATABASE gone');
$node->safe_psql('gone', q{CREATE EXTENSION test_prolly;
	CREATE TABLE g (a int, b text);
	INSERT INTO g SELECT g, repeat('g', 200) FROM generate_series(1, 3000) g;
	DROP TABLE g});
cmp_ok($node->safe_psql('gone', $pending), '>', 0, 'a database\'s queue holds a drop');
sql('DROP DATABASE gone');
is(sql(q{SELECT count(*) FROM pg_database WHERE datname = 'gone'}), '0',
	'and the database drops with it');

catch_up();
is($standby->safe_psql('postgres', $free), sql($free),
	'the standby\'s allocator is the primary\'s at the end');
is($standby->safe_psql('postgres', $pending), sql($pending),
	'and its queue');
$node->stop('immediate');
$node->start;
is(sql($free), $standby->safe_psql('postgres', $free),
	'and recovery agrees');

$standby->stop;
$node->stop;
done_testing();
