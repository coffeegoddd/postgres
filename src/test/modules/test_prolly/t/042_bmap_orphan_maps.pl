# Copyright (c) 2026, PostgreSQL Global Development Group

# Block-mapped storage made by a transaction that never commits.  A share
# made in a transaction raises the counts of every block it shares; if the
# server crashes before the transaction ends, no commit or abort record
# drops the copy, and the counts stay raised for good.  Its map names the
# transaction that made it, so the storage of one that is neither in
# progress nor committed is found and dropped: by autovacuum's storage
# maintenance after a start, or test_bmap_drop_orphans().  Storage of a
# transaction still in progress, of a committed one, or made outside any
# transaction is left alone.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('orphan_maps');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
log_min_messages = debug1
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 20000) g;
	CHECKPOINT;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
my $db = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
my $base = $node->data_dir . "/base/$db";
my $refs = qq{SELECT string_agg(refs::text, ',' ORDER BY blkno) FROM test_bmap_map($t)};
is(sql($refs) =~ /[^0,]/ ? 'shared' : 'own', 'own', 't\'s blocks are its own');

# A share in a transaction, left open as the server crashes.
my $open = $node->background_psql('postgres');
$open->query_safe(q{BEGIN});
my $copy = $open->query_safe(qq{SELECT test_bmap_share($t, true)});
ok(-f "$base/${copy}_bmap", 'the uncommitted share has its map');
$open->query_safe(q{CHECKPOINT});
isnt(sql($refs) =~ /^0(,0)*$/ ? 'own' : 'shared', 'own', 't\'s blocks are shared with it');

# Also left alone: a share outside any transaction (a test's copy), and
# one that commits.
my $kept = sql(qq{SELECT test_bmap_share($t)});
my $committed = sql(qq{BEGIN; SELECT test_bmap_share($t, true); COMMIT;});
$committed = (split /\n/, $committed)[-1] if $committed =~ /\n/;

# While the transaction is open, its storage is no orphan.
is(sql(q{SELECT test_bmap_drop_orphans()}), '0',
	'storage of a transaction in progress is not dropped');
ok(-f "$base/${copy}_bmap", 'and keeps its map');

$node->stop('immediate');
eval { $open->quit };
$node->start;
ok(-f "$base/${copy}_bmap", 'after the crash, the never-committed share\'s map is still there');

# Dropped by storage maintenance after the start, at its commit.
is(sql(q{SELECT test_bmap_drop_orphans()}), '1',
	'it is found as the storage of a transaction that never committed');
ok(!-f "$base/${copy}_bmap", 'and dropped');
ok(-f "$base/${kept}_bmap", 'a copy made outside any transaction is left');
ok($committed eq '' || -f "$base/${committed}_bmap", 'and so is one that committed');
is(sql(q{SELECT test_bmap_drop_orphans()}), '0', 'nothing is left to drop');

# Its holds are let go of: once the other copies go too, t's blocks are
# its own again, and dropping t frees every block.
sql(qq{SELECT test_bmap_unlink($kept)});
sql(qq{SELECT test_bmap_unlink($committed)}) if $committed ne '';
sql(q{SELECT * FROM test_bmap_release_drain()});
is(sql($refs) =~ /^0(,0)*$/ ? 'own' : 'shared', 'own',
	't\'s blocks are its own again: the orphan\'s holds were let go of');
my $free_before = sql(q{SELECT test_bmap_free_blocks()});
sql(q{DROP TABLE t});
sql(q{SELECT * FROM test_bmap_release_drain()});
cmp_ok(sql(q{SELECT test_bmap_free_blocks()}), '>', $free_before,
	'and dropping t frees its blocks');

# The same through autovacuum, whose storage maintenance after a start
# drops what a crash orphaned.
sql(q{
	CREATE TABLE u (a int, b text);
	INSERT INTO u SELECT g, repeat('y', 200) FROM generate_series(1, 5000) g;
});
my $u = sql(q{SELECT pg_relation_filenode('u')});
$open = $node->background_psql('postgres');
$open->query_safe(q{BEGIN});
my $ucopy = $open->query_safe(qq{SELECT test_bmap_share($u, true)});
$open->query_safe(q{CHECKPOINT});
$node->stop('immediate');
eval { $open->quit };
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1
});
$node->start;
ok($node->poll_query_until('postgres', qq{SELECT NOT EXISTS (SELECT 1 FROM pg_ls_dir('base/$db') f WHERE f = '${ucopy}_bmap')}),
	'autovacuum\'s storage maintenance drops what the crash orphaned');
ok($node->log_contains(qr/dropping storage "base\/$db\/$ucopy" made by transaction \d+, which did not commit/),
	'saying so');

$node->stop;
done_testing();
