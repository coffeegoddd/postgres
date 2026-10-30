# Copyright (c) 2026, PostgreSQL Global Development Group

# A VACUUM gives the shared pages of a block-mapped table, which say they
# are all-visible and are all-frozen, their visibility map bits back with
# XLOG_HEAP2_VM_SET records, of the map page alone, copying none of the
# pages.  Here the map was emptied (pg_truncate_visibility_map()) after the
# table was frozen.  A standby replays the records and so does crash
# recovery, and logical decoding passes over them as it does over any
# physical heap2 record: a slot reading past them decodes the changes
# after.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('vm_set_primary');
$primary->init(allows_streaming => 'logical');
# The map pages stay in the primary's buffers until it crashes, so only
# replaying the records sets the bits again.
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
});
$primary->start;
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('vm_set_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_visibility;
	CREATE EXTENSION pg_walinspect;
	CREATE TABLE plain (id int PRIMARY KEY);
	CREATE TABLE t (id int, pad text);
	INSERT INTO t SELECT g, repeat('v', 200) FROM generate_series(1, 3000) g;
	VACUUM (FREEZE) t;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
my $nblocks = sql(q{SELECT pg_relation_size('t') / 8192});
my $frozen = q{SELECT count(*) FROM pg_visibility_map('t')
	WHERE all_visible AND all_frozen};
is(sql($frozen), $nblocks, 'the table is all frozen');
sql(q{SELECT pg_truncate_visibility_map('t')});
is(sql($frozen), '0', 'and its map emptied');
my $copy = sql(qq{SELECT test_bmap_share($t)});
my $shared = qq{SELECT count(*) FROM test_bmap_map($t) x
	JOIN test_bmap_map($copy) y USING (blkno, phys)};
is(sql($shared), $nblocks, 'every page is shared');

sql(q{SELECT pg_create_logical_replication_slot('s', 'test_decoding')});
my $before = sql(q{SELECT pg_current_wal_insert_lsn()});
sql(q{VACUUM t});
# (WAL flushed by a transaction that logs a change: the VACUUM's commits none)
sql(q{CREATE TABLE flushed ()});
my $after = sql(q{SELECT pg_current_wal_flush_lsn()});
is( sql(
		qq{SELECT count(*) FROM pg_get_wal_records_info('$before', '$after')
	WHERE resource_manager = 'Heap2' AND record_type = 'VM_SET'}),
	$nblocks,
	'the VACUUM writes a VM_SET record for each page');
is(sql($frozen), $nblocks, 'which sets the bits again');
is(sql($shared), $nblocks, 'copying no page');

# Logical decoding passes over the records.
sql(q{INSERT INTO plain VALUES (42)});
my ($ret, $out, $err) = $primary->psql('postgres',
	q{SELECT data FROM pg_logical_slot_get_changes('s', NULL, NULL,
	  'include-xids', '0', 'skip-empty-xacts', '1')});
is($ret, 0, 'the slot decodes past the VM_SET records') or diag($err);
like(
	$out,
	qr/table public\.plain: INSERT: id\[integer\]:42/,
	'and the insert after them');

# A standby replays them.
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', $frozen),
	$nblocks, 'the standby has the bits');

# So does crash recovery.
$primary->stop('immediate');
$primary->start;
is(sql($frozen), $nblocks, 'the bits survive a crash');
is(sql($shared), $nblocks, 'and every page is still shared');
is(sql(q{SELECT count(*) FROM t}), '3000', 'with its rows');

sql(q{SELECT pg_drop_replication_slot('s')});
$standby->stop;
$primary->stop;
done_testing();
