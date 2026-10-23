# Copyright (c) 2026, PostgreSQL Global Development Group

# Block maps are trees: a relation's map is a tree of pages of the page
# store, under a root page of its own.  Extension makes nodes, truncation
# lets go of entries, and drop frees what the relation held, with every
# record replayed by a standby and after a crash, records checked by
# wal_consistency_checking.  A crash at a node's record leaves a tree that
# holds together.  A leaf's version on disk newer than the replay keeps its
# later entries.  A dropped tree's map, read before it is unlinked, points
# to no node its drop freed.  A map of an older format is refused.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('bmap_tree');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
shared_buffers = 16MB
checkpoint_timeout = 1h
wal_keep_size = '1GB'
wal_consistency_checking = 'all'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	 CREATE EXTENSION pg_buffercache');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('bmap_tree_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub digest
{
	my ($n, $rel) = @_;
	return $n->safe_psql('postgres',
		qq{SELECT count(*) || ':' || coalesce(sum(a), 0) || ':' ||
			coalesce(md5(string_agg(b, ',' ORDER BY a)), '') FROM $rel});
}

sub standby_agrees
{
	my ($what, $rel) = @_;
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
	is(digest($standby, $rel), digest($node, $rel),
		"$what: the standby reads $rel as the primary does");
}

sub sweep_frees
{
	return sql(q{SELECT freed FROM prolly_page_sweep()});
}

# a table over two subtrees: the leaf of blocks [0, 509), and 3 leaves of
# the next subtree, under one node
sql(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 10);
	INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 6000) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
is(sql(qq{SELECT string_agg(level || '\@' || first, ' ' ORDER BY first, level)
	FROM test_bmap_tree($rel)}),
	'0@0 0@509 1@509 0@1018 0@1527', 'its nodes: a leaf, and a node over three');
standby_agrees('extension', 't');

# changes, a fold's copies, a truncation, past a checkpoint and a crash
sql('CHECKPOINT');
sql(qq{SELECT test_bmap_fold_begin($rel)});
sql(q{UPDATE t SET b = 'u' || a WHERE a % 7 = 0});
sql(q{DELETE FROM t WHERE a > 4000});
sql(q{VACUUM t});
my $before = digest($node, 't');
standby_agrees('changes and a truncation', 't');
is(sweep_frees(), '0', 'a sweep finds nothing held by nothing');
$node->stop('immediate');
$node->start;
is(digest($node, 't'), $before, 'the table recovers after a crash');
is(sweep_frees(), '0', 'and still holds every block it had');

# a crash just after a node's record, before the extension that wanted it
my $ext = $node->background_psql('postgres');
$ext->query_safe(q{SELECT injection_points_set_local()});
$ext->query_safe(q{SELECT injection_points_attach('bmap-tree-node-made', 'wait')});
$ext->query_until(qr/extending/, q{\echo extending
INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(10001, 14000) g;
});
$node->wait_for_event('client backend', 'bmap-tree-node-made');
# (another commit flushes the node's record)
sql('CREATE TABLE flush_wal ()');
$node->stop('immediate');
$ext->{run}->finish;
$node->start;
is(digest($node, 't'), $before, 'a crash at a node made leaves the rows');
is(sweep_frees(), '0', 'the node made is the tree\'s, held');
sql(q{INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(10001, 14000) g});
$before = digest($node, 't');
standby_agrees('extension after the crash', 't');

# a leaf's newer version on disk, under an older image replayed over it:
# the remaps after the image are kept
sql('CHECKPOINT');
sql(q{UPDATE t SET b = 'v' WHERE a = 3});
sql(q{SELECT test_bmap_fold_begin(pg_relation_filenode('t'))});
sql(q{UPDATE t SET b = 'w' || a WHERE a % 3 = 0});
$before = digest($node, 't');
sql(q{SELECT count(*) FROM pg_buffercache
	WHERE relfilenode = 9421 AND (pg_buffercache_evict(bufferid)).buffer_evicted});
$node->stop('immediate');
$node->start;
is(digest($node, 't'), $before, 'newer leaves on disk keep their later entries');
is(sweep_frees(), '0', 'and hold every block');
standby_agrees('remaps', 't');

# drop: every block the table held freed; its map, read while the drop
# waits between freeing the blocks and unlinking the map, points to none of
# them, though they are made other pages and nodes meanwhile.  Its changes
# list (of the folds above) still lists blocks it held, made another
# table's nodes by then: they are counted held, and so are their children.
my $free_before = sql(q{SELECT test_bmap_free_blocks()});
my $drop = $node->background_psql('postgres');
$drop->query_safe(q{SELECT injection_points_set_local()});
$drop->query_safe(q{SELECT injection_points_attach('bmap-dropped', 'wait')});
$drop->query_until(qr/dropping/, q{\echo dropping
DROP TABLE t;
});
$node->wait_for_event('client backend', 'bmap-dropped');
cmp_ok(sql(q{SELECT test_bmap_free_blocks()}), '>', $free_before,
	'its blocks are free');
sql(q{CREATE TABLE r (a int, b text) WITH (fillfactor = 10);
	INSERT INTO r SELECT g, repeat('r', 200) FROM generate_series(1, 6000) g});
is(sql(qq{SELECT count(*) FROM test_bmap_tree($rel)}), '0',
	'the dropped tree\'s map points to no node');
is(sweep_frees(), '0', 'a sweep reads the dropped tree as holding nothing');
# (the table's toast relations, dropped after it, don't wait)
$node->safe_psql('postgres', q{SELECT injection_points_detach('bmap-dropped')});
$node->safe_psql('postgres', q{SELECT injection_points_wakeup('bmap-dropped')});
$drop->quit;
sql(q{DROP TABLE r});
is(sweep_frees(), '0', 'dropping the trees freed all they held');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

# a map of an older format (the flat maps of version 1) is refused
sql(q{CREATE TABLE old (a int); INSERT INTO old VALUES (1)});
my $old = sql(q{SELECT pg_relation_filenode('old')});
my $path = $node->data_dir . '/'
  . sql(q{SELECT pg_relation_filepath('old')}) . '_bmap';
$node->stop;
open(my $fh, '+<:raw', $path) or die "open $path: $!";
seek($fh, 24, 0) or die "seek $path: $!";
print $fh pack('L', 1) or die "write $path: $!";
close($fh) or die "close $path: $!";
$node->start;
# (the header's checksum no longer matches)
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	q{SET ignore_checksum_failure = on; SELECT count(*) FROM old});
like($stderr, qr/format version 1, which this server does not read/,
	'a map of version 1 is refused');

$standby->stop;
$node->stop;
done_testing();
