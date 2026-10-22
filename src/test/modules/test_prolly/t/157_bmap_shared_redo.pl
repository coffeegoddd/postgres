# Copyright (c) 2026, PostgreSQL Global Development Group

# A base backup can copy a page-store block before it is shared, and its
# source's map after a later remap.  Recovery skips the source's older WAL
# through that newer map, but must still reconstruct the shared copy (#665).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('shared_redo');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
wal_consistency_checking = 'all'
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly; CREATE EXTENSION pg_buffercache;
	CREATE EXTENSION pg_walinspect});
sql(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 50);
	INSERT INTO t SELECT g, repeat('x', 500) FROM generate_series(1, 150) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
my $key = sql(q{SELECT a FROM t WHERE ctid = '(10,1)'});
my $phys = sql(qq{SELECT phys FROM test_bmap_map($rel) WHERE blkno = 10});
my $store = $node->data_dir . '/base/'
  . sql(q{SELECT oid FROM pg_database WHERE datname = current_database()})
  . '/9421';

sql('CHECKPOINT');
open(my $fh, '<:raw', $store) or die "open $store: $!";
seek($fh, $phys * 8192, 0) or die "seek $store: $!";
my $before;
read($fh, $before, 8192) == 8192 or die "short read of $store";
close($fh) or die "close $store: $!";

# An image followed by delta updates: none can be recovered through the
# source's later map entry.  The copy must retain all three updates.
for my $value (1 .. 3)
{
	sql(qq{UPDATE t SET b = repeat('$value', 500) WHERE a = $key});
}
is(sql(qq{SELECT (ctid::text::point)[0] FROM t WHERE a = $key}), '10',
	'the updated row stays on the block');
is(sql(qq{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted)
	FROM pg_buffercache WHERE relfilenode = $rel AND relforknumber = 0}),
	't', 'sharing reads the source pages after eviction');
my $copy = sql(qq{SELECT test_bmap_share($rel)});
my $rows = sql(qq{SELECT test_bmap_rows_digest($copy)});

# Repeated sharing of an immutable page must work too.
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
my $copy2 = sql(qq{SELECT test_bmap_share($copy)});
my $end = sql(q{SELECT pg_current_wal_insert_lsn()});
is(sql(qq{SELECT count(*) FROM pg_get_wal_records_info('$start', '$end')
	WHERE resource_manager = 'BlockMap' AND record_type = 'PRESERVE'}),
	'0', 'sharing an already shared page logs no further preservation images');
is(sql(qq{SELECT test_bmap_rows_digest($copy2)}), $rows,
	'the second copy has the same rows');
sql(qq{SELECT test_bmap_truncate($rel, 10)});
sql(qq{SELECT test_bmap_extend($rel, 1)});
isnt(sql(qq{SELECT phys FROM test_bmap_map($rel) WHERE blkno = 10}), $phys,
	'the source gets a different physical block');
is(sql(qq{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted)
	FROM pg_buffercache WHERE relfilenode = $rel AND relforknumber = 4}),
	't', 'the newer source map reaches disk');
$node->stop('immediate');

# Reproduce the two reads of a fuzzy backup deterministically: the older
# store page and newer map.  Keep the page's valid checksum, of logical
# block 10, so success also checks physical-address replay's checksum.
open($fh, '+<:raw', $store) or die "open $store: $!";
seek($fh, $phys * 8192, 0) or die "seek $store: $!";
print $fh $before or die "write $store: $!";
close($fh) or die "close $store: $!";

$node->start;
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $rows,
	'the shared copy recovers all changes missing from the backup page');
is(sql(qq{SELECT test_bmap_rows_digest($copy2)}), $rows,
	'the second copy recovers the same shared page');
$node->stop;

done_testing();
