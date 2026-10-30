# Copyright (c) 2026, PostgreSQL Global Development Group

# With WAL skipped for new relations (wal_level=minimal), a share's copy
# keeps its source's visibility map through a crash: the map's pages are
# the tree's, shared as the relation's are and made durable by the share,
# which logs no image of them.  The copy gets no free space map.  And a
# table made and filled in one transaction with COPY FREEZE, its pages and
# its map's bits written without WAL, keeps both through a crash once it
# commits, as its pages are logged at commit, the map's among them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('share_maps_minimal');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
wal_level = minimal
max_wal_senders = 0
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_walinspect;
	CREATE EXTENSION pg_visibility;
	CREATE TABLE t (a int8, b int8);
	INSERT INTO t SELECT g, g FROM generate_series(1, 20000) g;
	VACUUM (FREEZE) t;
	CHECKPOINT;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
my $dir = $node->data_dir . '/base/'
  . sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
ok(-s "$dir/${t}_fsm", 'the source has a free space map');
ok(!-e "$dir/${t}_vm", 'and its visibility map is no file of its own');

my $before = sql(q{SELECT pg_current_wal_insert_lsn()});
my $copy = sql(qq{SELECT test_bmap_share($t)});
# (WAL flushed by a transaction that logs a change: the helper's commits none)
sql(q{CREATE TABLE flushed ()});
is( sql(
		qq{SELECT count(*)
	FROM pg_get_wal_block_info('$before', pg_current_wal_insert_lsn())
	WHERE relfilenode = $copy AND block_fpi_length > 0}
	),
	'0',
	'the share logs no image of the copy\'s pages');
ok(!-e "$dir/${copy}_fsm", 'the copy has no free space map');
is( sql(
		qq{SELECT count(*) FROM test_bmap_map($t, 2) s
	JOIN test_bmap_map($copy, 2) c USING (blkno) WHERE s.phys = c.phys}),
	sql(qq{SELECT count(*) FROM test_bmap_map($t, 2)}),
	'its visibility map\'s pages are the source\'s');

$node->stop('immediate');
$node->start;

# The copy made a table, so that the catalogs reach it.
sql(qq{
	CREATE TABLE c (LIKE t);
	UPDATE pg_class SET relfilenode = $copy WHERE relname = 'c';
});
my $vm = q{SELECT string_agg(all_visible::int::text || all_frozen::int::text, '' ORDER BY blkno) FROM pg_visibility_map};
is(sql(qq{$vm('c')}), sql(qq{$vm('t')}),
	'after a crash, the copy\'s map is the source\'s');
like(sql(qq{$vm('c')}), qr/^(11)+$/, 'every page all-visible and all-frozen');
is(sql(q{SELECT count(*) FROM pg_check_frozen('c')}), '0',
	'and every page is');
is(sql(qq{SELECT test_bmap_rows_digest($copy) = test_bmap_rows_digest($t)}),
	't', 'the copy keeps its rows');

# A table made and filled with COPY FREEZE in one transaction.
sql(q{
	BEGIN;
	CREATE TABLE f (a int8, b int8);
	COPY f FROM PROGRAM 'seq 1 20000 | sed -e "s/.*/&\t&/"' WITH (FREEZE);
	COMMIT;
});
my $f_vm = sql(qq{$vm('f')});
# (all but the last ones, which COPY leaves, as it does in md.c's files)
like($f_vm, qr/^(11)+(00)*$/,
	'COPY FREEZE sets its pages all-visible and all-frozen');
$node->stop('immediate');
$node->start;
is(sql(qq{$vm('f')}), $f_vm, 'and a crash keeps the map');
is(sql(q{SELECT count(*), sum(a) FROM f}), '20000|200010000', 'and the rows');

$node->stop;
done_testing();
