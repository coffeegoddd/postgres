# Copyright (c) 2026, PostgreSQL Global Development Group

# A share of a block-mapped relation costs the same whatever the relation's
# size, its visibility map included: the map's pages are shared with the
# relation's own, copied before either relation changes one, and the free
# space map, a hint that would have to be copied, is left for the copy to
# build again.  The copy's visibility map is the source's, so index-only
# scans of it skip the heap and VACUUM skips its all-visible pages; it stays
# correct as either relation changes, and through a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('share_maps');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 4GB
shared_buffers = 128MB
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_walinspect;
	CREATE EXTENSION pg_visibility;
});

# A table of 'pages' pages of two rows each, frozen and all-visible, its
# free space map and visibility map built by the VACUUM.
sub make_table
{
	my ($name, $pages) = @_;

	sql(qq{
		CREATE TABLE $name (a int, b text);
		ALTER TABLE $name ALTER COLUMN b SET STORAGE PLAIN;
		INSERT INTO $name SELECT g, repeat('x', 3000)
			FROM generate_series(1, $pages * 2) g;
		VACUUM (FREEZE) $name;
	});
	return sql(qq{SELECT pg_relation_filenode('$name')});
}

# The WAL a share of relation file 'filenode' logs: its records' bytes,
# images left out, the images, and the images of the copy's pages, of any
# fork.  (Images logged to set hint bits on the catalogs are left out.)
sub share_wal
{
	my ($filenode) = @_;
	my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
	my @out = split /\n/, sql(qq{
		SELECT test_bmap_share($filenode);
		SELECT pg_current_wal_insert_lsn()});
	my ($copy, $end) = @out;

	# (flushed: a share outside a transaction doesn't flush its WAL)
	sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
	my ($bytes, $fpis, $copy_fpis) = split /\|/, sql(qq{
		SELECT (SELECT coalesce(sum(record_length - fpi_length), 0)
				FROM pg_get_wal_records_info('$start', '$end')
				WHERE record_type <> 'FPI_FOR_HINT'),
			(SELECT coalesce(sum(fpi_length), 0)
				FROM pg_get_wal_records_info('$start', '$end')
				WHERE record_type <> 'FPI_FOR_HINT'),
			(SELECT count(*) FROM pg_get_wal_block_info('$start', '$end', false)
			 WHERE relfilenode = $copy AND block_fpi_length > 0)});
	return ($copy, $bytes, $fpis, $copy_fpis);
}

# The time a share takes, the least of a few, in milliseconds.
sub share_time
{
	my ($filenode) = @_;
	my $best;

	for (1 .. 5)
	{
		my $out = '';
		$node->psql('postgres', qq{
			DO \$\$
			DECLARE t0 timestamptz := clock_timestamp();
			BEGIN
				PERFORM test_bmap_share($filenode);
				RAISE NOTICE 'ms %', extract(epoch FROM clock_timestamp() - t0) * 1000;
			END \$\$;
		}, stderr => \$out);
		$out =~ /ms ([0-9.]+)/ or die "no time in $out";
		$best = $1 if !defined $best || $1 < $best;
	}
	return $best;
}

my $small = make_table('small', 500);
my $large = make_table('large', 50000);
my $warm = make_table('warm', 1);
sql(q{CHECKPOINT});
share_wal($warm);

my ($small_copy, $small_bytes, $small_fpis, $small_copy_fpis) =
  share_wal($small);
my ($large_copy, $large_bytes, $large_fpis, $large_copy_fpis) =
  share_wal($large);
my $small_ms = share_time($small);
my $large_ms = share_time($large);
note "a share of 500 pages logs $small_bytes bytes and $small_fpis of images "
  . "in ${small_ms} ms; of 50000 pages $large_bytes bytes and $large_fpis of "
  . "images in ${large_ms} ms";

cmp_ok($large_bytes, '<=', $small_bytes + 256,
	'a share of a table a hundred times as large logs the same WAL');
cmp_ok($large_fpis, '<=', $small_fpis,
	'and no more images');
is($small_copy_fpis + $large_copy_fpis, 0,
	'none of them of the copy\'s pages, of any fork');
cmp_ok($large_ms, '<=', 2 * $small_ms + 5,
	'and takes about as long');

# The copy made a table of its own, so the catalogs can reach it.
sub adopt
{
	my ($name, $like, $copy) = @_;

	sql(qq{
		CREATE TABLE $name (LIKE $like);
		UPDATE pg_class SET relfilenode = $copy,
			relfrozenxid = (SELECT relfrozenxid FROM pg_class WHERE relname = '$like'),
			relminmxid = (SELECT relminmxid FROM pg_class WHERE relname = '$like')
			WHERE relname = '$name';
	});
}
adopt('large_copy', 'large', $large_copy);

my $summary = q{SELECT all_visible || ',' || all_frozen FROM pg_visibility_map_summary};
is(sql(qq{$summary('large_copy')}), sql(qq{$summary('large')}),
	'the copy\'s visibility map is the source\'s');
is(sql(qq{$summary('large_copy')}), '50000,50000', 'every page all-visible and all-frozen');
is(sql(q{SELECT count(*) FROM pg_check_visible('large_copy')}), '0',
	'and every page it says is all-visible is');
is(sql(q{SELECT count(*) FROM pg_check_frozen('large_copy')}), '0',
	'and every page it says is all-frozen is');

# Index-only scans of the copy skip the heap.
sql(q{CREATE INDEX large_copy_a ON large_copy (a)});
sub heap_fetches
{
	my ($where) = @_;
	my $plan = sql(qq{
		SET enable_seqscan = off; SET enable_bitmapscan = off;
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
		SELECT count(a) FROM large_copy WHERE $where});
	$plan =~ /Index Only Scan/ or die "not an index-only scan:\n$plan";
	$plan =~ /Heap Fetches: (\d+)/ or die "no heap fetches in:\n$plan";
	return $1;
}
is(heap_fetches('a <= 20000'), 0, 'an index-only scan of the copy reads no heap page');

# A change of the copy's pages clears the copy's bits alone, and a change of
# the source's the source's alone.
sql(q{DELETE FROM large_copy WHERE a % 1000 = 0});
sql(q{UPDATE large SET a = -a WHERE a % 1000 = 1});
is(sql(q{SELECT count(*) FROM large_copy WHERE a <= 20000}), '19980',
	'the copy\'s rows after a change');
cmp_ok(heap_fetches('a <= 20000'), '>', 0, 'the pages it changed are read from the heap');
is(sql(qq{$summary('large_copy')}), '49900,49900',
	'the copy\'s pages changed are no longer all-visible');
is(sql(qq{$summary('large')}), '49900,49900',
	'nor are the source\'s, its own changes\' alone');
is(sql(q{SELECT count(*) FROM pg_check_visible('large_copy')}), '0',
	'the copy\'s map is still right');
is(sql(q{SELECT count(*) FROM pg_check_visible('large')}), '0',
	'and so is the source\'s');
is(sql(q{SELECT count(*) FROM pg_visibility('large_copy') WHERE all_visible <> pd_all_visible}), '0',
	'and the copy\'s map agrees with its pages');

# The copy knows of no free space until vacuumed: then inserts reuse it.
sql(q{VACUUM (INDEX_CLEANUP ON) large_copy});
is(sql(qq{$summary('large_copy')}), '50000,50000',
	'VACUUM sets the copy\'s changed pages all-visible (and, freezing eagerly, all-frozen) again');
my $copy_pages = sql(q{SELECT pg_relation_size('large_copy') / 8192});
sql(q{INSERT INTO large_copy SELECT -g, repeat('y', 3000) FROM generate_series(1, 100) g});
is(sql(q{SELECT pg_relation_size('large_copy') / 8192}), $copy_pages,
	'inserts after VACUUM go to the free space it found');

# A crash keeps both maps as they were.
my $copy_vm = sql(q{SELECT string_agg(all_visible::int::text || all_frozen::int::text, '' ORDER BY blkno) FROM pg_visibility_map('large_copy')});
my $src_vm = sql(q{SELECT string_agg(all_visible::int::text || all_frozen::int::text, '' ORDER BY blkno) FROM pg_visibility_map('large')});
$node->stop('immediate');
$node->start;
is(sql(q{SELECT string_agg(all_visible::int::text || all_frozen::int::text, '' ORDER BY blkno) FROM pg_visibility_map('large_copy')}),
	$copy_vm, 'the copy\'s map after a crash');
is(sql(q{SELECT string_agg(all_visible::int::text || all_frozen::int::text, '' ORDER BY blkno) FROM pg_visibility_map('large')}),
	$src_vm, 'and the source\'s');
is(sql(q{SELECT count(*) FROM pg_check_visible('large_copy')}), '0',
	'the copy\'s map is right after a crash');

# A map page copied as it changes logs its image, where a heap page that
# changed since the redo pointer logs none: a bit cleared with a heap
# change leaves the map page's LSN as it was, so its LSN doesn't tell
# whether its changes are all in the WAL.  (A VACUUM after the checkpoint
# sets a bit again, logging the map page; a change after the share then
# clears another, copying the page.)  The change is made with copies
# without an image allowed, as every copy logs one by default.
my $vm_imaged = make_table('vm_imaged', 10);
sql(q{CHECKPOINT; UPDATE vm_imaged SET b = 'y' WHERE a = 1; VACUUM vm_imaged;});
my ($vm_imaged_copy) = share_wal($vm_imaged);
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
sql(q{SET block_map_copy_images = off; UPDATE vm_imaged SET b = 'z' WHERE a = 7});
sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
is(sql(qq{
	SELECT count(*) || ',' || count(*) FILTER (WHERE block_fpi_length > 0)
	FROM pg_get_wal_block_info('$start', pg_current_wal_insert_lsn(), false)
	WHERE resource_manager = 'BlockMap' AND record_type = 'REMAP'
	  AND relfilenode = $vm_imaged AND relforknumber = 2}),
	'1,1', 'a copy of a map page logs its image');

# (A standby's maps after a share, a crash of it and its promotion: t/036.)
$node->stop;
done_testing();
