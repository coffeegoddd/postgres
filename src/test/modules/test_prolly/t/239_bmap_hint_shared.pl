# Copyright (c) 2026, PostgreSQL Global Development Group

# Hints and data checksums on block-mapped pages.  A hint set on a page
# another map shares doesn't dirty it: the shared block is never written in
# place, so no torn write can reach it, and each holder reads it as it was
# synced when it came to be shared.  A hint set on a relation's own page is
# protected as on heap: with checksums its first change after a checkpoint
# logs a full-page image, and a torn write of the page is repaired by redo.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('hint_shared');
$node->init(extra => ['--data-checksums']);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
shared_buffers = 64MB
});
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sql(q{CREATE EXTENSION test_prolly; CREATE EXTENSION pg_buffercache});
is(sql('SHOW data_checksums'), 'on', 'data checksums are on');

my $dboid = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
my $store = $node->data_dir . "/base/$dboid/9421";

sub block_bytes
{
	my ($phys) = @_;
	open(my $fh, '<:raw', $store) or die "open $store: $!";
	seek($fh, $phys * 8192, 0) or die "seek: $!";
	my $n = read($fh, my $buf, 8192);
	close($fh);
	die "short read" unless $n == 8192;
	return $buf;
}

# A table shared before its rows' hints are set: reading it sets them, on
# pages its copy holds too.
sql(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 50);
	INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 2000) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
sql('CHECKPOINT');
my $copy = sql(qq{SELECT test_bmap_share($rel)});
my $phys = sql(qq{SELECT phys FROM test_bmap_map($rel) WHERE blkno = 3});
is(sql(qq{SELECT phys FROM test_bmap_map($copy) WHERE blkno = 3}), $phys,
	'the copy holds the same block');
my $before = block_bytes($phys);
sql(q{SELECT count(*) FROM t});
is(sql(qq{SELECT count(*) FROM pg_buffercache WHERE relfilenode = $rel AND isdirty}),
	'0', 'hints set on shared pages dirty none of them');
sql('CHECKPOINT');
is(block_bytes($phys) eq $before ? 'same' : 'changed', 'same',
	'and the shared block is not written in place');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), sql(qq{SELECT test_bmap_rows_digest($rel)}),
	'both read the same rows');

# A relation's own page: its hint's first change after a checkpoint logs an
# image, and a torn write of the page is repaired by redo.
sql(q{CREATE TABLE own (a int, b text) WITH (fillfactor = 50);
	INSERT INTO own SELECT g, repeat('o', 100) FROM generate_series(1, 2000) g});
my $orel = sql(q{SELECT pg_relation_filenode('own')});
sql('CHECKPOINT');
my $want = sql(q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM own});
my $lsn = sql('SELECT pg_current_wal_insert_lsn()');
# (hints set, and the pages written out, a hint's image logged first)
sql(q{SELECT count(*) FROM own});
cmp_ok(sql(qq{SELECT count(*) FROM pg_buffercache WHERE relfilenode = $orel AND isdirty}),
	'>', 0, 'hints dirty the relation\'s own pages');
cmp_ok(sql(qq{SELECT pg_current_wal_insert_lsn() - '$lsn'::pg_lsn}), '>', 8192,
	'and log their images');
sql(qq{SELECT count(*) FROM pg_buffercache WHERE relfilenode = $orel
	AND (pg_buffercache_evict(bufferid)).buffer_evicted});
my $ophys = sql(qq{SELECT phys FROM test_bmap_map($orel) WHERE blkno = 5});
$node->stop('immediate');

# a torn write: the block's second half from before the hint, garbage
open(my $fh, '+<:raw', $store) or die "open $store: $!";
seek($fh, $ophys * 8192 + 4096, 0) or die "seek: $!";
print $fh ("\xAB" x 4096) or die "write: $!";
close($fh) or die "close: $!";
$node->start;
is(sql(q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM own}),
	$want, 'redo repairs a torn write of an own page from its hint\'s image');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), sql(qq{SELECT test_bmap_rows_digest($rel)}),
	'and the shared pages read as before');

# (a control: the same tear of a page with no image to repair it, its
# hints written out by a checkpoint, is caught by its checksum)
my $cphys = sql(qq{SELECT phys FROM test_bmap_map($orel) WHERE blkno = 7});
sql('CHECKPOINT');
$node->stop('immediate');
open($fh, '+<:raw', $store) or die "open $store: $!";
seek($fh, $cphys * 8192 + 4096, 0) or die "seek: $!";
print $fh ("\xAB" x 4096) or die "write: $!";
close($fh) or die "close: $!";
$node->start;
my ($ret, $out, $err) = $node->psql('postgres', q{SELECT count(*) FROM own});
like($err, qr/invalid page/, 'a tear redo has no image for is caught by the checksum');

$node->stop;
done_testing();
