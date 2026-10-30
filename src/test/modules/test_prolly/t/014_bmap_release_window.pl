# Copyright (c) 2026, PostgreSQL Global Development Group

# A relation dropped at commit is queued for release before the commit
# record, its entries naming the transaction.  A crash after the commit,
# before the drop itself, leaves the entries there to drain: nothing leaks
# for a sweep to find.  Until the drop is done (its buffers dropped) the
# entries hold nothing and a drain stops at them.  A transaction that fails
# or crashes after queuing, before its commit, keeps its table: a drain
# pops its entries and frees nothing.
#
# An extension that would grow the page store drains the queue first, so
# dropped storage is reused.  A standby replays it all.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('bmap_release_window');
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
my $standby = PostgreSQL::Test::Cluster->new('bmap_release_window_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub catch_up
{
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
}

my $free = q{SELECT test_bmap_free_blocks()};
my $pending = q{SELECT test_bmap_release_pending()};
my $store = q{SELECT (pg_stat_file('base/' || oid || '/9421')).size / 8192
	FROM pg_database WHERE datname = current_database()};

sub make_table
{
	my ($name, $rows) = @_;
	sql(qq{CREATE TABLE $name (a int, b int8) WITH (fillfactor = 10);
		INSERT INTO $name SELECT g, g FROM generate_series(1, $rows) g});
	return sql(qq{SELECT pg_relation_size('$name') / 8192});
}

sub drain
{
	return sql(q{SELECT entries || '/' || freed FROM test_bmap_release_drain()});
}

sub sweep_frees
{
	return sql(q{SELECT freed FROM test_bmap_sweep()});
}

# a session stopped at an injection point while it runs 'query'
sub stopped_at
{
	my ($point, $query) = @_;
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->query_safe(qq{SELECT injection_points_set_local();
		SELECT injection_points_attach('$point', 'wait')});
	$s->query_until(qr/running/, qq{
		\\echo running
		$query;
	});
	$node->wait_for_event('client backend', $point);
	return $s;
}

sub crash
{
	my ($s) = @_;
	$node->stop('immediate');
	eval { $s->quit };
	$node->start;
}

# A crash after a drop's commit, before the drop itself: its entries were
# queued before the commit record, and a drain frees what the table held.
my $blocks = make_table('a', 20000);
cmp_ok($blocks, '>', 1000, 'the table has a thousand blocks and more');
sql('CHECKPOINT');
is(sql($pending), '0', 'the queue is empty');
my $s = stopped_at('bmap-predrop', 'DROP TABLE a');
cmp_ok(sql($pending), '>', 0, 'the committed drop\'s entries are queued');
is(drain(), '0/0', 'a drain takes none while the drop is in flight');
crash($s);
cmp_ok(sql($pending), '>', 0, 'after a crash, the entries are still queued');
my ($entries, $freed) = split m{/}, drain();
cmp_ok($freed, '>=', $blocks, 'and a drain frees what the table held');
is(sweep_frees(), '0', 'nothing is left for a sweep');
is(sql($pending), '0', 'the queue is empty');

# A sweep while a drop's relation has its map emptied, before the drop's
# end: the queue's entries hold the table's blocks, and the sweep frees
# none of them; the drain after frees them once.
make_table('w', 20000);
my $wblocks = sql(q{SELECT pg_relation_size('w') / 8192});
sql('CHECKPOINT');
$s = stopped_at('bmap-dropped', 'DROP TABLE w');
is(sweep_frees(), '0', 'a sweep after the drop emptied the map frees none of it');
sql(q{SELECT injection_points_wakeup('bmap-dropped')});
$s->quit;
($entries, $freed) = split m{/}, drain();
cmp_ok($freed, '>=', $wblocks, 'the drain after frees the table');
is(sweep_frees(), '0', 'and nothing is left, nor freed twice');
is(sql(q{SELECT count(*) FROM test_bmap_release_drain()}), '1', 'the store reads well');

# A failure after the entries are queued, before the commit: the table
# stays; a drain pops the entries and frees nothing.
make_table('b', 5000);
my $rows = q{SELECT count(*) FROM b};
sql('CHECKPOINT');
my $free_before = sql($free);
my ($ret, $out, $err) = $node->psql('postgres', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-drop-prequeued', 'error');
	DROP TABLE b;
});
like($err, qr/bmap-drop-prequeued/, 'the drop failed after queuing');
unlike($err, qr/you don't own a lock/, 'and let go of its locks quietly');
is(sql($rows), '5000', 'the table stays');
cmp_ok(sql($pending), '>', 0, 'its entries are queued');
($entries, $freed) = split m{/}, drain();
cmp_ok($entries, '>', 0, 'a drain pops them');
is($freed, '0', 'and frees nothing');
is(sql($free), $free_before, 'no block of the table is freed');

# A crash after the entries are queued, before the commit: the same.
$s = stopped_at('bmap-drop-prequeued', 'DROP TABLE b');
cmp_ok(sql($pending), '>', 0, 'a drop yet to commit has its entries queued');
is(drain(), '0/0', 'a drain takes none while it may still commit');
sql('SELECT txid_current()');
crash($s);
is(sql($rows), '5000', 'after a crash before the commit, the table stays');
($entries, $freed) = split m{/}, drain();
cmp_ok($entries, '>', 0, 'a drain pops its entries');
is($freed, '0', 'and frees nothing');
is(sweep_frees(), '0', 'nothing leaks');
is(sql(q{SELECT sum(a) FROM b}), '12502500', 'the table reads as it was');

# A table loaded after a drop reuses the blocks the drop left queued: the
# store doesn't grow by the table.
make_table('c', 20000);
sql('CHECKPOINT');
my $store_before = sql($store);
sql('DROP TABLE c');
my $queued = sql($pending);
cmp_ok($queued, '>', 0, 'the dropped table is queued');
my $cblocks = make_table('d', 20000);
is(sql($pending), '0', 'the load drained the queue');
cmp_ok(sql($store) - $store_before, '<', $cblocks / 10,
	'and the store grew by less than a tenth of the table');
is(sql(q{SELECT count(*) FROM d}), '20000', 'the table holds its rows');

# the standby replays it all: its queue and free blocks are the primary's
catch_up();
is($standby->safe_psql('postgres', $pending), sql($pending),
	'the standby\'s queue is the primary\'s');
is($standby->safe_psql('postgres', $free), sql($free),
	'and so are its free blocks');
is($standby->safe_psql('postgres', q{SELECT count(*) FROM d}), '20000',
	'and it reads the table');

# a crash after: the queue and the blocks as they were, nothing leaks
my ($p, $f) = (sql($pending), sql($free));
$node->stop('immediate');
$node->start;
is(sql($pending) . '/' . sql($free), "$p/$f", 'a crash keeps the queue and blocks');
drain();
is(sweep_frees(), '0', 'drained, nothing leaks');
is(sql(q{SELECT count(*) FROM d}), '20000', 'and the table holds its rows');

$standby->stop;
$node->stop;
done_testing();
