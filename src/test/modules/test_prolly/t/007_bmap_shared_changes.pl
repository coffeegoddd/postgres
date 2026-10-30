# Copyright (c) 2026, PostgreSQL Global Development Group

# Pages a block-mapped relation shares with a copy: what may reach them and
# what copies them.  A hint true of the relation alone stays off them;
# VACUUM copies only the pages it changes; a cleanup lock that isn't
# granted copies nothing; a page zeroed to be
# initialized is copied without an image, and its new block reads as
# zeroes, after a crash too.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('shared_changes');
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
	CREATE EXTENSION pg_buffercache;
	CREATE EXTENSION pageinspect;
	CREATE EXTENSION pg_walinspect;
	CREATE SCHEMA cow;
});
is(sql(q{SHOW data_checksums}), 'on', 'hints are logged');

# An index entry killed by a scan of the relation, whose rows a copy still
# has: the hint stays in the relation's buffer.
sql(q{
	CREATE TABLE cow.k (a int PRIMARY KEY);
	INSERT INTO cow.k SELECT g FROM generate_series(1, 2000) g;
	CHECKPOINT;
});
my $kidx = sql(q{SELECT pg_relation_filenode('cow.k_pkey')});
my $kcopy = sql(qq{SELECT test_bmap_share($kidx)});
my $kdigest = sql(qq{SELECT test_bmap_digest($kcopy)});
sql(q{DELETE FROM cow.k WHERE a <= 300});
sql(q{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SELECT count(*) FROM cow.k WHERE a <= 300;
});
cmp_ok(sql(q{SELECT count(*) FROM bt_page_items('cow.k_pkey', 1) WHERE dead}),
	'>', 0, 'the scan killed entries');
sql(q{SELECT pg_buffercache_evict_relation('cow.k_pkey')});
sql(qq{SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache
	WHERE relfilenode = $kcopy});
is(sql(qq{SELECT test_bmap_digest($kcopy)}), $kdigest,
	'the copy\'s index pages are unchanged');
$node->stop('immediate');
$node->start;
is(sql(qq{SELECT test_bmap_digest($kcopy)}), $kdigest,
	'and after recovery');
sql(qq{SELECT test_bmap_unlink($kcopy)});

# VACUUM visits every page under a cleanup lock, and copies those it
# changes: the heap pages a delete left dead rows on, and the index's
# leaves that held them.
sql(q{
	CREATE TABLE cow.v (a int PRIMARY KEY);
	INSERT INTO cow.v SELECT g FROM generate_series(1, 20000) g;
	VACUUM (FREEZE) cow.v;
	CHECKPOINT;
});
my $v = sql(q{SELECT pg_relation_filenode('cow.v')});
my $vidx = sql(q{SELECT pg_relation_filenode('cow.v_pkey')});
my $hcopy = sql(qq{SELECT test_bmap_share($v)});
my $icopy = sql(qq{SELECT test_bmap_share($vidx)});
my $hpages = sql(qq{SELECT count(*) FROM test_bmap_map($v)});
my $ipages = sql(qq{SELECT count(*) FROM test_bmap_map($vidx)});
my $hdigest = sql(qq{SELECT test_bmap_rows_digest($hcopy)});
my $idigest = sql(qq{SELECT test_bmap_digest($icopy)});
cmp_ok($hpages, '>', 50, 'the heap has pages to share');
cmp_ok($ipages, '>', 20, 'so does the index');

sql(q{DELETE FROM cow.v WHERE a <= 1000});
sql(q{VACUUM (DISABLE_PAGE_SKIPPING) cow.v});
my $hshared = sql(qq{SELECT count(*) FROM test_bmap_map($v) WHERE refs > 0});
my $ishared = sql(qq{SELECT count(*) FROM test_bmap_map($vidx) WHERE refs > 0});
cmp_ok($hshared, '>=', $hpages - 6, 'VACUUM left the heap\'s other pages shared');
cmp_ok($ishared, '>=', $ipages - 6, 'and the index\'s');
is(sql(q{SELECT count(*), min(a) FROM cow.v}), '19000|1001',
	'the rows the delete left');
is(sql(qq{SELECT test_bmap_rows_digest($hcopy)}), $hdigest,
	'the copy\'s heap is unchanged');
is(sql(qq{SELECT test_bmap_digest($icopy)}), $idigest,
	'and its index');
sql(qq{SELECT test_bmap_unlink($hcopy)});
sql(qq{SELECT test_bmap_unlink($icopy)});

# A cleanup lock refused because another backend has the page pinned
# leaves the page shared.
sql(q{
	CREATE TABLE cow.p (a int);
	INSERT INTO cow.p SELECT g FROM generate_series(1, 100) g;
});
my $p = sql(q{SELECT pg_relation_filenode('cow.p')});
my $pcopy = sql(qq{SELECT test_bmap_share($p)});
my $pinner = $node->background_psql('postgres');
$pinner->query_safe(q{
	BEGIN;
	DECLARE c CURSOR FOR SELECT * FROM cow.p;
	FETCH 1 FROM c;
});
is(sql(qq{SELECT test_bmap_cleanup_lock($p, 0)}), 'f', 'the cleanup lock is refused');
is(sql(qq{SELECT refs FROM test_bmap_map($p) WHERE blkno = 0}), '1',
	'and the page is still shared');
$pinner->query_safe('COMMIT');
$pinner->quit;
is(sql(qq{SELECT test_bmap_cleanup_lock($p, 0)}), 't', 'once unpinned, it is granted');
is(sql(qq{SELECT refs FROM test_bmap_map($p) WHERE blkno = 0}), '0',
	'and copies the page');
sql(qq{SELECT test_bmap_unlink($pcopy)});

# A page zeroed to be initialized is copied to a new block without an
# image.  The new block is one a dropped relation left, which still holds
# that relation's page: it must read as zeroes, written out or recovered.
sql(q{
	CREATE TABLE cow.z (a int);
	INSERT INTO cow.z SELECT g FROM generate_series(1, 300) g;
	CREATE TABLE cow.junk (a int);
	INSERT INTO cow.junk SELECT -1 FROM generate_series(1, 200) g;
	CHECKPOINT;
	DROP TABLE cow.junk;
});
my $z = sql(q{SELECT pg_relation_filenode('cow.z')});
my $zcopy = sql(qq{SELECT test_bmap_share($z)});
my $zdigest = sql(qq{SELECT test_bmap_rows_digest($zcopy)});
my $zrest = sql(q{SELECT count(*) FROM cow.z WHERE ctid >= '(1,0)'});
sql(q{SELECT pg_buffercache_evict_relation('cow.z')});
my $before = sql(q{SELECT pg_current_wal_insert_lsn()});
sql(qq{SELECT test_bmap_zero_block($z, 0)});
# (WAL flushed by a transaction that logs a change: the helper's commits none)
sql(q{CREATE TABLE flushed_zero ()});
is(sql(qq{SELECT count(*), count(*) FILTER (WHERE block_fpi_length > 0)
	FROM pg_get_wal_block_info('$before', pg_current_wal_insert_lsn())
	WHERE relfilenode = $z AND relforknumber = 0}),
	'1|0', 'the copy logs the page without an image');
is(sql(qq{SELECT refs FROM test_bmap_map($z) WHERE blkno = 0}), '0',
	'the page went to a new block');

sql(q{CHECKPOINT});
sql(q{SELECT pg_buffercache_evict_relation('cow.z')});
is(sql(q{SELECT count(*), count(*) FILTER (WHERE a < 0) FROM cow.z}), "$zrest|0",
	'written out, the zeroed page has no rows');
is(sql(qq{SELECT test_bmap_rows_digest($zcopy)}), $zdigest, 'the copy keeps its rows');

# again, recovered: the new block is a dropped relation's, never written
sql(q{
	CREATE TABLE cow.junk2 (a int);
	INSERT INTO cow.junk2 SELECT -1 FROM generate_series(1, 200) g;
	CHECKPOINT;
	DROP TABLE cow.junk2;
});
sql(q{SELECT pg_buffercache_evict_relation('cow.z')});
sql(qq{SELECT test_bmap_zero_block($z, 1)});
sql(q{CREATE TABLE flushed ()});
$node->stop('immediate');
$node->start;
$node->restart;
is(sql(q{SELECT count(*) FROM cow.z WHERE a < 0}), '0',
	'recovered, the zeroed page has no rows');
is(sql(qq{SELECT test_bmap_rows_digest($zcopy)}), $zdigest, 'the copy keeps its rows');
sql(qq{SELECT test_bmap_unlink($zcopy)});

$node->stop;
done_testing();
