# Copyright (c) 2026, PostgreSQL Global Development Group

# A crash between a truncation and its end (bmapposttruncate()) leaves the
# entries past the relation's new length.  An extension past the length
# reuses them, zeroing their blocks, so it gives them a new 'since' too: a
# share syncs only the blocks a relation got before the checkpoint, and logs
# the others, and a block reused since the checkpoint holds a page made
# since.  Logged, its image is what recovery from the checkpoint ends with,
# though it replays the extension's zeroing of the block, now the copy's,
# and skips the source's records for the block, its map newer.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('reused_stale_since');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
block_map_standby_hold_max_size = 0
checkpoint_timeout = 1h
max_wal_size = 1GB
bgwriter_lru_maxpages = 0
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE EXTENSION pg_walinspect;
	CREATE EXTENSION pg_buffercache;
});

sub sql { return $node->safe_psql('postgres', $_[0]); }

# A table of 200 full pages, rows frozen; then its second half deleted and
# vacuumed, the truncation stopped by a crash before its end.
sql(q{
	CREATE TABLE t (a int, b text) WITH (fillfactor = 100);
	INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 200 * 34) g;
	VACUUM (FREEZE) t;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
my $pages = sql(q{SELECT pg_relation_size('t') / 8192});
sql(q{DELETE FROM t WHERE ctid >= '(100,0)'});
sql(q{SELECT injection_points_attach('bmap-truncated', 'wait')});
my $vacuum = $node->background_psql('postgres', on_error_stop => 0);
$vacuum->query_until(qr/started/, q{
	\echo started
	VACUUM t;
});
$node->wait_for_event('client backend', 'bmap-truncated');
$node->stop('immediate');
eval { $vacuum->quit };
$node->start;

my $length = sql(q{SELECT pg_relation_size('t') / 8192});
cmp_ok($length, '<', $pages, 'the truncation cut the table');
cmp_ok(sql(qq{SELECT count(*) FROM test_bmap_mapped($t) WHERE blkno >= $length}), '>', 0,
	'and left its entries past the new length, the crash before its end');

# After a checkpoint, rows that extend the table again over those entries.
sql(q{CHECKPOINT});
sql(q{INSERT INTO t SELECT g, repeat('y', 200) FROM generate_series(1, 50 * 34) g});
my $grown = sql(q{SELECT pg_relation_size('t') / 8192});
cmp_ok($grown, '>', $length, 'the table grew back over the reused entries');
my $digest = sql(qq{SELECT test_bmap_rows_digest($t)});

# The share logs the blocks reused since the checkpoint, one image each.
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
my $copy = sql(qq{SELECT test_bmap_share($t)});
sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
cmp_ok(sql(qq{
	SELECT count(DISTINCT relblocknumber) FROM pg_get_wal_block_info('$start', pg_current_wal_insert_lsn(), false)
	WHERE relfilenode = $t AND relforknumber = 0 AND block_fpi_length > 0
	  AND relblocknumber >= $length}), '>=', $grown - $length,
	'the share logs every block reused since the checkpoint');

# The source changed everywhere after the share, and every buffer written
# out, the maps too; then a crash, whose recovery begins at the checkpoint,
# before the extension.
sql(q{UPDATE t SET b = 'changed'});
sql(q{SELECT count(*) FROM pg_buffercache_evict_all()});
$node->stop('immediate');
$node->start;
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'the copy reads the rows the source had at the share, reused blocks too');

done_testing();
