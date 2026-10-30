# Copyright (c) 2026, PostgreSQL Global Development Group

# Sharing block-mapped storage at its limits: records that change as many
# reference-count pages as one record may, a share that fails partway, and
# a share made while WAL is skipped for new relations.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# a reference-count page's blocks: a page's bytes less its header and
# special space, eight bytes a block (a count and a roots' count)
my $refs_per_page = int((8192 - 24 - 8) / 8);

my $node = PostgreSQL::Test::Cluster->new('share_limits');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA cow;
	CREATE TABLE cow.s (a int);
	CREATE TABLE cow.f (a int);
});
my $s = sql(q{SELECT pg_relation_filenode('cow.s')});
my $f = sql(q{SELECT pg_relation_filenode('cow.f')});

# Each of s's 20 blocks on its own reference-count page: a filler is
# extended past a page's worth of blocks between them.
for my $i (1 .. 20)
{
	sql(qq{SELECT test_bmap_extend($s, 1)});
	sql(qq{SELECT test_bmap_extend($f, $refs_per_page)});
}
is(sql(qq{SELECT count(DISTINCT phys / $refs_per_page) FROM test_bmap_map($s)}),
	'20', 'the blocks are on 20 reference-count pages');

# Sharing them counts a reference on each page in one record, as does
# dropping a copy, and letting a block go that a truncation left past the
# end when the relation grows again.
my $c1 = sql(qq{SELECT test_bmap_share($s)});
my $c2 = sql(qq{SELECT test_bmap_share($s)});
is(sql(qq{SELECT string_agg(DISTINCT refs::text, ',') FROM test_bmap_map($s)}),
	'2', 'two copies counted');
sql(qq{SELECT test_bmap_unlink($c1)});
is(sql(qq{SELECT string_agg(DISTINCT refs::text, ',') FROM test_bmap_map($s)}),
	'1', 'one copy dropped');
my $shared = sql(qq{SELECT string_agg(phys::text, ',' ORDER BY blkno) FROM test_bmap_map($s)});
sql(qq{SELECT test_bmap_truncate($s, 0)});
sql(qq{SELECT test_bmap_extend($s, 20)});
is(sql(qq{SELECT string_agg(DISTINCT refs::text, ',') FROM test_bmap_map($c2)}),
	'0', 'the grown relation let the shared blocks go');
is(sql(qq{SELECT count(*) FROM test_bmap_map($s) WHERE phys = ANY ('{$shared}'::int8[])}),
	'0', 'and has new ones');

# Crash recovery replays the records (flushed by a transaction that logs a
# change: the helpers' transactions commit none).
my $c2_map = sql(qq{SELECT string_agg(blkno || ':' || phys || ':' || refs, ',' ORDER BY blkno)
	FROM test_bmap_map($c2)});
sql(q{CREATE TABLE flushed ()});
$node->stop('immediate');
$node->start;
is(sql(qq{SELECT string_agg(blkno || ':' || phys || ':' || refs, ',' ORDER BY blkno)
	FROM test_bmap_map($c2)}),
	$c2_map, 'recovery replays them');
sql(qq{SELECT test_bmap_unlink($c2)});
sql(q{DROP TABLE cow.f});

# A share that fails partway, after some of the source's blocks were
# counted for the copy, gives them back when the transaction aborts.
SKIP:
{
	skip 'injection points not supported by this build', 2
	  if $ENV{enable_injection_points} ne 'yes';

	sql(q{CREATE EXTENSION injection_points});
	sql(q{CREATE TABLE cow.big (a int)});
	my $big = sql(q{SELECT pg_relation_filenode('cow.big')});
	# more than one map page
	sql(qq{SELECT test_bmap_extend($big, 3000)});
	sql(q{SELECT injection_points_attach('bmap-share-map-page', 'error')});
	my ($ret, $out, $err) = $node->psql('postgres',
		qq{BEGIN; SELECT test_bmap_share($big, true); COMMIT;});
	like($err, qr/error triggered for injection point bmap-share-map-page/,
		'the share failed partway');
	sql(q{SELECT injection_points_detach('bmap-share-map-page')});
	# (the aborted copy's storage, queued, let go of)
	sql(q{SELECT * FROM test_bmap_release_drain()});
	is(sql(qq{SELECT count(*) FROM test_bmap_map($big) WHERE refs <> 0}),
		'0', 'no block is left counted for the aborted copy');
}

$node->stop;

# With WAL skipped for new relations, committing a copy doesn't log or
# write its shared pages again: the share itself logs the source's own
# blocks as images, once each.
my $min = PostgreSQL::Test::Cluster->new('share_minimal');
$min->init;
$min->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
wal_level = minimal
max_wal_senders = 0
});
$min->start;

sub msql { return $min->safe_psql('postgres', $_[0]); }

msql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA cow;
	CREATE TABLE cow.t (a int8, b int8, c int8, d int8);
	INSERT INTO cow.t SELECT g, g, g, g FROM generate_series(1, 10000) g;
	CHECKPOINT;
});
my $t = msql(q{SELECT pg_relation_filenode('cow.t')});
my $pages = msql(qq{SELECT count(*) FROM test_bmap_map($t)});
cmp_ok($pages, '>', 50, 'the source has pages to share');
my $before = msql(q{SELECT pg_current_wal_insert_lsn()});
my $copy = msql(qq{
	BEGIN;
	SELECT test_bmap_share($t, true);
	COMMIT;
});
my $wal = msql(qq{SELECT pg_current_wal_insert_lsn() - '$before'::pg_lsn});
cmp_ok($wal, '>', $pages * 8192 / 2, 'the share logs the source\'s own pages as images');
cmp_ok($wal, '<', $pages * 8192 * 5 / 4, 'once each: the commit logs no image of the copy\'s pages');
is(msql(qq{SELECT count(*) FROM test_bmap_map($copy) c JOIN test_bmap_map($t) s
	USING (blkno, phys)}),
	$pages, 'the copy still shares every page');

# The rows survive a crash, in the copy and in the source.  The pages are
# compared as rows, without hint bits: a reader (a scan) may set
# HEAP_XMIN_COMMITTED on the shared pages in their buffers after the share
# wrote them, and a crash may lose that, as it may any hint, with no row
# changed.
my $rows = msql(qq{SELECT test_bmap_rows_digest($t)});
my $source_rows = msql(q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM cow.t});
$min->stop('immediate');
$min->start;
is(msql(qq{SELECT test_bmap_rows_digest($copy)}), $rows, 'and survives a crash');
is(msql(qq{SELECT test_bmap_rows_digest($t)}), $rows,
	'as does the source\'s storage');
is(msql(q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM cow.t}),
	$source_rows, 'and its rows');

$min->stop;
done_testing();
