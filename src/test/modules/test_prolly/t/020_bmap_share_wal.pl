# Copyright (c) 2026, PostgreSQL Global Development Group

# What sharing block-mapped storage costs in WAL, and what the first write
# to a shared page costs.
#
# A share must log O(1) WAL whatever the size of the relation: logging a
# full-page image of each page the relation owns makes branching a large
# table cost as much WAL as copying it.  A page copied before its first
# change after a share must not log its image again when an image of it is
# in the WAL since the redo pointer already.  And the first write to a
# shared page after a checkpoint must cost about what the first write to an
# unshared page costs then: its image, with no read or write of the page
# besides, and a bounded number of map lookups.

use strict;
use warnings FATAL => 'all';
use JSON::PP;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('share_wal');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 1GB
shared_buffers = 64MB
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_walinspect;
});

# A table of about 'pages' pages, its rows frozen so that no hint bit is
# set later, written out by a checkpoint.
sub make_table
{
	my ($name, $pages) = @_;

	sql(qq{
		CREATE TABLE $name (a int, b text) WITH (fillfactor = 50);
		INSERT INTO $name SELECT g, repeat('x', 200)
			FROM generate_series(1, $pages * 17) g;
		VACUUM (FREEZE) $name;
	});
	return sql(qq{SELECT pg_relation_filenode('$name')});
}

# The WAL bytes 'query' logs, and the full-page images of relation file
# 'filenode''s main fork among its records.  The records counted are those
# from where it begins to where it ends, taken in its session: those of
# other sessions in between are few (autovacuum is off), but for images
# logged to set hint bits on the catalogs, which are left out.
sub wal_of
{
	my ($query, $filenode) = @_;
	my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
	my @out = split /\n/, sql(qq{$query; SELECT pg_current_wal_insert_lsn()});
	my $end = $out[-1];

	# (flushed: a share outside a transaction doesn't flush its WAL)
	sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
	return split /\|/, sql(qq{
		SELECT (SELECT coalesce(sum(record_length), 0)
				FROM pg_get_wal_records_info('$start', '$end')
				WHERE record_type <> 'FPI_FOR_HINT'),
			(SELECT count(*) FROM pg_get_wal_block_info('$start', '$end', false)
			 WHERE relfilenode = $filenode AND relforknumber = 0 AND block_fpi_length > 0)});
}

# A share of a table ten times as large logs the same WAL, and no image of
# the table's pages.  (A share of another table first logs the images of
# the catalog pages a share changes first after the checkpoint.)
my $small = make_table('small', 100);
my $large = make_table('large', 1000);
my $warm = make_table('warm', 1);
my $small_pages = sql(q{SELECT pg_relation_size('small') / 8192});
my $large_pages = sql(q{SELECT pg_relation_size('large') / 8192});
sql(q{CHECKPOINT});
sql(qq{SELECT test_bmap_share($warm)});
my ($small_bytes, $small_images) = wal_of(qq{SELECT test_bmap_share($small)}, $small);
my ($large_bytes, $large_images) = wal_of(qq{SELECT test_bmap_share($large)}, $large);
note "a share of $small_pages pages logs $small_bytes bytes, of $large_pages pages $large_bytes";
cmp_ok($large_bytes, '<=', $small_bytes + 64,
	'a share of a table ten times as large logs the same WAL');
is($small_images, 0, 'no image of the small table\'s pages');
is($large_images, 0, 'and no image of the large table\'s pages');

# Pages changed since the checkpoint have their images in the WAL since
# the redo pointer: neither the share nor the copies made by the next
# change log them again.  Each counted apart, as each is a lesson of its
# own.
my $imaged = make_table('imaged', 100);
sql(q{CHECKPOINT});
sql(q{UPDATE imaged SET a = a WHERE a % 17 = 1});
my (undef, $share_images) =
  wal_of(qq{SELECT test_bmap_share($imaged)}, $imaged);
my (undef, $copy_images) =
  wal_of(q{UPDATE imaged SET a = a WHERE a % 17 = 2}, $imaged);
is($share_images, 0,
	'a share logs no image of pages imaged since the redo pointer');
is(sql(qq{SELECT count(*) FROM test_bmap_map($imaged) WHERE refs = 0}),
	sql(q{SELECT pg_relation_size('imaged') / 8192}),
	'the change copied every page');
is($copy_images, 0, 'and the copies after it log none either');

# The first write to each page after a checkpoint: of a table none shares
# and of one that is shared.  The shared table's pages are copied, which
# logs about the WAL the unshared table's images take, reads and writes
# no page, and costs a bounded number of buffer lookups for each page.
my $plain = make_table('plain', 200);
my $shared = make_table('shared', 200);
sql(qq{SELECT test_bmap_share($shared)});
sql(q{CHECKPOINT});

sub first_writes
{
	my ($table) = @_;
	my $plan = decode_json(sql(qq{
		EXPLAIN (ANALYZE, BUFFERS, WAL, COSTS OFF, TIMING OFF, FORMAT JSON)
		UPDATE $table SET a = a WHERE a % 17 = 3}))->[0]{Plan};

	return $plan;
}
my $p = first_writes('plain');
my $s = first_writes('shared');
my $pages = sql(q{SELECT pg_relation_size('shared') / 8192});

cmp_ok($p->{'WAL FPI'}, '>=', $pages - 1, 'each unshared page logs an image');
cmp_ok($s->{'WAL Bytes'}, '<=', 1.25 * $p->{'WAL Bytes'},
	'the copies of shared pages log about as much WAL as the unshared pages\' images');
is($s->{'Shared Read Blocks'} + $s->{'Local Read Blocks'}, 0,
	'the copies read no page');
is($s->{'Shared Written Blocks'}, 0, 'and write none out');
cmp_ok(($s->{'Shared Hit Blocks'} - $p->{'Shared Hit Blocks'}) / $pages, '<=', 16,
	'and look up a bounded number of buffers for each page copied');

$node->stop;
done_testing();
