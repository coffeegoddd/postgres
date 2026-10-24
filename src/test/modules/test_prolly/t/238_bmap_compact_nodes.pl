# Copyright (c) 2026, PostgreSQL Global Development Group

# A page sweep moves tree nodes down, safely for redo from before the move:
# a change logged before the move, its page still only in shared buffers,
# survives a crash whether the moved node's parent reached disk or not,
# and whether the node's old block was taken again since.  A standby
# replays the moves and survives a crash of its own.  No node moves while
# an online backup runs, and a backup that overlaps a move (on the primary,
# or on a standby replaying it) fails at its stop.  Every page is checked
# against the primary's in replay (wal_consistency_checking).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('compact_nodes');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
wal_keep_size = '1GB'
shared_buffers = 128MB
bgwriter_lru_maxpages = 0
restart_after_crash = on
wal_consistency_checking = 'all'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	 CREATE EXTENSION pg_buffercache');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('compact_nodes_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $sweep = q{SELECT moved FROM prolly_page_sweep()};

# Two tables extended in turn, so their blocks (and the first's nodes,
# made as it grows) alternate in the store; the second is dropped.
sub interleave
{
	my ($keep, $gone, $rounds) = @_;
	sql(qq{CREATE TABLE $keep (a int PRIMARY KEY, b text) WITH (fillfactor = 10);
		CREATE TABLE $gone (a int, b text) WITH (fillfactor = 10)});
	sql(qq{DO \$\$
		BEGIN
			FOR i IN 1 .. $rounds LOOP
				INSERT INTO $keep SELECT g, repeat('k', 300)
					FROM generate_series(i * 10, i * 10 + 9) g;
				INSERT INTO $gone SELECT g, repeat('g', 300)
					FROM generate_series(i * 10, i * 10 + 9) g;
			END LOOP;
		END \$\$});
	sql(qq{DROP TABLE $gone});
}

sub digest
{
	my ($t, $n) = @_;
	$n //= $node;
	return $n->safe_psql('postgres',
		qq{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM $t});
}

sub nodes
{
	my ($t) = @_;
	return sql(qq{SELECT string_agg(block::text, ',' ORDER BY block)
		FROM test_bmap_tree(pg_relation_filenode('$t'))});
}

# (a first sweep, so that the next moves only what the next drop leaves)
interleave('base', 'base_gone', 100);
sql($sweep);

# A table whose leaf was copied to the store's end (a snapshot shared it
# when its first page changed) while its other pages stay low: the move
# leaves those pages where they are, under the moved leaf.
sub copied_leaf
{
	my ($t) = @_;
	sql(qq{CREATE TABLE $t (a int PRIMARY KEY, b text) WITH (fillfactor = 10);
		INSERT INTO $t SELECT g, repeat('c', 300) FROM generate_series(1, 800) g});
	my $rel = sql(qq{SELECT pg_relation_filenode('$t')});
	my $snap = sql(qq{SELECT test_bmap_snapshot_take($rel)});
	sql(qq{UPDATE $t SET b = 'first' WHERE a = 1});
	sql(qq{SELECT test_bmap_snapshot_release($rel, $snap)});
	sql(q{SELECT * FROM test_bmap_release_drain()});
	return $rel;
}

# A change logged after a checkpoint to a page under the leaf, its page
# only in shared buffers, then the leaf moved and the store shortened past
# its old block: a crash recovers the change (the copy and the parent were
# made durable in that order).
copied_leaf('c1');
my $c1nodes = nodes('c1');
sql('CHECKPOINT');
# (twice: the first change logs the pages' images, the second only deltas,
# which redo applies through the map)
sql(q{UPDATE c1 SET b = 'pre' WHERE a IN (401, 501, 601, 701);
	UPDATE c1 SET b = 'changed' WHERE a IN (401, 501, 601, 701)});
my $wantc1 = digest('c1');
sql($sweep);
isnt(nodes('c1'), $c1nodes, 'the copied leaf moves down');
$node->stop('immediate');
$node->start;
is(digest('c1'), $wantc1, 'a crash after the leaf moved keeps the change from before it');

# The same on a standby, from a restartpoint before the change: its replay
# makes the copy and the parent durable as the primary did.
copied_leaf('c2');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
sql('CHECKPOINT');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
$standby->safe_psql('postgres', 'CHECKPOINT');
# (twice: the first change logs the pages' images, the second only deltas,
# which redo applies through the map)
sql(q{UPDATE c2 SET b = 'pre' WHERE a IN (401, 501, 601, 701);
	UPDATE c2 SET b = 'changed' WHERE a IN (401, 501, 601, 701)});
my $wantc2 = digest('c2');
sql($sweep);
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
$standby->stop('immediate');
$standby->start;
is(digest('c2', $standby), $wantc2,
	'a standby crashing after replaying the move keeps the change from before it');

# A change logged after a checkpoint, its page only in shared buffers, then
# a sweep moving the table's nodes: a crash recovers the change.
interleave('t1', 't1_gone', 300);
sql('CHECKPOINT');
sql(q{UPDATE t1 SET b = 'changed' WHERE a % 10 = 3});
my $want1 = digest('t1');
my $nodes1 = nodes('t1');
cmp_ok(sql($sweep), '>', 0, 'a sweep moves blocks');
isnt(nodes('t1'), $nodes1, 'the table\'s tree nodes among them');
$node->stop('immediate');
$node->start;
is(digest('t1'), $want1, 'a crash after the move keeps the change from before it');

# The same, with the old nodes' blocks taken by another table and written
# out before the crash: redo never reads them as the nodes.
interleave('t2', 't2_gone', 300);
sql('CHECKPOINT');
sql(q{UPDATE t2 SET b = 'changed' WHERE a % 10 = 7});
my $want2 = digest('t2');
my $nodes2 = nodes('t2');
sql($sweep);
my $now2 = nodes('t2');
isnt($now2, $nodes2, 'the second table\'s nodes move');
my %now = map { $_ => 1 } split /,/, $now2;
my @old = grep { !$now{$_} } split /,/, $nodes2;
sql(q{CREATE TABLE reuse (a int, b text) WITH (fillfactor = 10);
	INSERT INTO reuse SELECT g, repeat('r', 300) FROM generate_series(1, 4000) g});
my $old_list = join(',', @old);
sql(qq{SELECT count(*) FROM pg_buffercache WHERE relfilenode = 9421
	AND relblocknumber IN ($old_list) AND (pg_buffercache_evict(bufferid)).buffer_evicted});
$node->stop('immediate');
$node->start;
is(digest('t2'), $want2, 'a crash after the old blocks are reused keeps the change');
is(sql(q{SELECT count(*) FROM reuse}), '4000', 'and the table that took them reads');

# A standby replays the moves, and survives a crash of its own.
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is(digest('t1', $standby), $want1, 'the standby replays the first moves');
is(digest('t2', $standby), $want2, 'and the second');
$standby->stop('immediate');
$standby->start;
is(digest('t2', $standby), $want2, 'and after a crash of its own, reads the same');

# No node moves while an online backup runs; one that started as the
# moves were about to begin fails at its stop, even before their old
# blocks are freed.
copied_leaf('t3');
my $nodes3 = nodes('t3');
my $b = $node->background_psql('postgres', on_error_stop => 0);
$b->query_safe(q{SET client_min_messages = warning});
$b->query_safe(q{SELECT pg_backup_start('during', true)});
sql($sweep);
is(nodes('t3'), $nodes3, 'no node moves while a backup runs');
$b->query_safe(q{SELECT * FROM pg_backup_stop()});
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-compact-nodes', 'wait')});
$s->query_until(qr/sweeping/, qq{\\echo sweeping
	SELECT injection_points_attach('bmap-compact-nodes-installed', 'wait');
	$sweep;
});
$node->wait_for_event('client backend', 'bmap-compact-nodes');
$b->query_safe(q{SELECT pg_backup_start('overlapping', true)});
$node->safe_psql('postgres', q{SELECT injection_points_detach('bmap-compact-nodes');
	SELECT injection_points_wakeup('bmap-compact-nodes')});
# (stopped once the nodes have moved, before their old blocks are freed)
$node->wait_for_event('client backend', 'bmap-compact-nodes-installed');
$b->query(q{SELECT * FROM pg_backup_stop()});
like($b->{stderr}, qr/moved tree nodes during online backup/,
	'a backup that overlapped the moves fails at its stop, during the sweep');
$node->safe_psql('postgres', q{SELECT injection_points_detach('bmap-compact-nodes-installed');
	SELECT injection_points_wakeup('bmap-compact-nodes-installed')});
$s->quit;
isnt(nodes('t3'), $nodes3, 'the sweep moves the nodes');
$b->quit;

# On a standby, a backup during which a move is replayed fails at its stop.
interleave('t4', 't4_gone', 300);
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
my $sb = $standby->background_psql('postgres', on_error_stop => 0);
$sb->query_safe(q{SET client_min_messages = warning});
$sb->query_safe(q{SELECT pg_backup_start('standby', true)});
my $nodes4 = nodes('t4');
sql($sweep);
isnt(nodes('t4'), $nodes4, 'the primary moves the nodes');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
$sb->query(q{SELECT * FROM pg_backup_stop()});
like($sb->{stderr}, qr/moved tree nodes during online backup/,
	'a standby backup during the moves\' replay fails at its stop');
$sb->quit;
is(digest('t4', $standby), digest('t4'), 'the standby reads the moved table');

# A standby backup started after a replayed move, from a restartpoint past
# it (a checkpoint on the primary, replayed, then one on the standby),
# succeeds.
sql('CHECKPOINT');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
$standby->safe_psql('postgres', 'CHECKPOINT');
$sb = $standby->background_psql('postgres', on_error_stop => 0);
$sb->query_safe(q{SET client_min_messages = warning});
$sb->query_safe(q{SELECT pg_backup_start('after', true)});
$sb->query(q{SELECT * FROM pg_backup_stop()});
unlike($sb->{stderr}, qr/moved tree nodes/,
	'a standby backup from a restartpoint past the moves succeeds');
$sb->quit;
$standby->stop;

# The changes a crash must keep: made after a checkpoint, twice (images,
# then deltas), on pages under a leaf the sweep moves.
my $changes = q{UPDATE %s SET b = 'pre' WHERE a IN (401, 501, 601, 701);
	UPDATE %s SET b = 'changed' WHERE a IN (401, 501, 601, 701)};
sub changed
{
	my ($t) = @_;
	sql('CHECKPOINT');
	sql(sprintf($changes, $t, $t));
	return digest($t);
}

# A crash, then a sweep and another crash: the table as it was, each time.
sub crash_keeps
{
	my ($t, $want, $what) = @_;
	$node->stop('immediate');
	$node->start;
	is(digest($t), $want, "$what: a crash keeps the changes");
	sql($sweep);
	$node->stop('immediate');
	$node->start;
	is(digest($t), $want, "$what: and a sweep and a crash after");
}

# The copy's block taken from a table that had it after the checkpoint
# (its extension zeroing it, its pages written there), dropped before the
# sweep: redo of those records leaves the copy alone.
copied_leaf('p');
{
	# (a sweep under a backup: the data pages move, the leaf doesn't)
	my $bk = $node->background_psql('postgres');
	$bk->query_safe(q{SET client_min_messages = warning});
	$bk->query_safe(q{SELECT pg_backup_start('p', true)});
	sql($sweep);
	$bk->query_safe(q{SELECT * FROM pg_backup_stop()});
	$bk->quit;
}
my $pnodes = nodes('p');
sql('CHECKPOINT');
sql(q{CREATE TABLE q (a int, b text) WITH (fillfactor = 10);
	INSERT INTO q SELECT g, repeat('q', 300) FROM generate_series(1, 2) g});
sql(sprintf($changes, 'p', 'p'));
my $wantp = digest('p');
sql(q{DROP TABLE q; SELECT * FROM test_bmap_release_drain()});
sql($sweep);
isnt(nodes('p'), $pnodes, 'the leaf moves to a block another table had since the checkpoint');
crash_keeps('p', $wantp, 'a copy on a block with recent records');

# A crash at each step of a move.
for my $point (qw(bmap-compact-nodes bmap-compact-node-written
	bmap-compact-node-moved bmap-compact-nodes-installed
	bmap-compact-parents-synced))
{
	my $t = 'x_' . ($point =~ s/-/_/gr);
	copied_leaf($t);
	my $want = changed($t);
	my $w = $node->background_psql('postgres', on_error_stop => 0);
	$w->query_safe(qq{SELECT injection_points_set_local();
		SELECT injection_points_attach('$point', 'wait')});
	$w->query_until(qr/sweeping/, qq{\\echo sweeping
		$sweep;
	});
	$node->wait_for_event('client backend', $point);
	$node->stop('immediate');
	eval { $w->quit };
	$node->start;
	is(digest($t), $want, "a crash at $point keeps the changes");
	sql($sweep);
	$node->stop('immediate');
	$node->start;
	is(digest($t), $want, "and a sweep and a crash after it");
}

# A cancel while a move is being finished waits for it (the parents
# synced), and cancels the sweep; the next sweep frees the old block.
copied_leaf('e1');
my $wante1 = changed('e1');
my $e1nodes = nodes('e1');
my $w = $node->background_psql('postgres', on_error_stop => 0);
$w->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-compact-node-moved', 'wait')});
my $pid = $w->query_safe('SELECT pg_backend_pid()');
$w->query_until(qr/sweeping/, qq{\\echo sweeping
	$sweep;
});
$node->wait_for_event('client backend', 'bmap-compact-node-moved');
sql(qq{SELECT pg_cancel_backend($pid)});
# (a cancel that ended the wait leaves nothing to wake)
$node->psql('postgres', q{SELECT injection_points_wakeup('bmap-compact-node-moved')});
$w->quit;
isnt(nodes('e1'), $e1nodes, 'a cancelled sweep finishes the move it began');
# (the next sweep frees and cuts off the old block before any crash)
sql($sweep);
$node->stop('immediate');
$node->start;
is(digest('e1'), $wante1, 'after a cancelled sweep, a sweep and a crash keep the changes');

# An error while a move is being finished: the server restarts, and redo
# finishes it.
copied_leaf('e2');
my $wante2 = changed('e2');
my ($ret, $out, $err) = $node->psql('postgres', qq{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-compact-node-moved', 'error');
	$sweep;
});
like($err, qr/server closed the connection|terminated/,
	'an error while finishing a move is a PANIC');
$node->poll_query_until('postgres', 'SELECT true');
is(digest('e2'), $wante2, 'and recovery keeps the changes');
sql($sweep);
$node->stop('immediate');
$node->start;
is(digest('e2'), $wante2, 'and after a sweep and a crash');

$node->stop;
done_testing();
