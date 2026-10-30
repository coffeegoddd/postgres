# Copyright (c) 2026, PostgreSQL Global Development Group

# A share leaves its copy no free space map, and storage made from a
# captured root none of its own: the copy's VACUUM finds the free space
# again.  The copy's visibility map is its source's, so VACUUM would skip
# every page that was all-visible when shared and never find their free
# space, and inserts into the copy would extend it for good.  So VACUUM
# reads the pages it skips, while the storage manager says the free space
# map may not know them, only to record their free space, once.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('share_free_space');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 1GB
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_buffercache;
	CREATE EXTENSION pg_visibility;
});

# A table of 1000 pages of two 3000-byte rows each, about 2 kB free on
# every page, all-visible and all-frozen.
sub make_table
{
	my ($name) = @_;

	sql(qq{
		CREATE TABLE $name (a int, b text);
		ALTER TABLE $name ALTER COLUMN b SET STORAGE PLAIN;
		INSERT INTO $name SELECT g, repeat('x', 3000)
			FROM generate_series(1, 2000) g;
		VACUUM (FREEZE) $name;
	});
	return sql(qq{SELECT pg_relation_filenode('$name')});
}

# Storage 'copy' made a table of its own, so the catalogs can reach it.
sub adopt
{
	my ($name, $like, $copy) = @_;

	sql(qq{
		CREATE TABLE $name (LIKE $like);
		ALTER TABLE $name ALTER COLUMN b SET STORAGE PLAIN;
		UPDATE pg_class SET relfilenode = $copy,
			relpages = (SELECT relpages FROM pg_class WHERE relname = '$like'),
			relfrozenxid = (SELECT relfrozenxid FROM pg_class WHERE relname = '$like'),
			relminmxid = (SELECT relminmxid FROM pg_class WHERE relname = '$like')
			WHERE relname = '$name';
	});
}

sub pages { return sql(qq{SELECT pg_relation_size('$_[0]') / 8192}); }

# 500 rows of 1500 bytes, which fit the free space of the pages.
sub fill
{
	sql(qq{INSERT INTO $_[0] SELECT -g, repeat('y', 1500) FROM generate_series(1, 500) g});
}

# The buffers VACUUM (VERBOSE) of 'table' reads, every buffer evicted first.
sub vacuum_reads
{
	my ($table) = @_;
	my $out = '';

	sql(q{SELECT count(pg_buffercache_evict(bufferid)) FROM pg_buffercache});
	$node->psql('postgres', qq{VACUUM (VERBOSE) $table}, stderr => \$out);
	$out =~ /buffer usage: \d+ hits, (\d+) reads/
	  or die "no buffer usage in:\n$out";
	return $1;
}

my $s = make_table('s');
is(pages('s'), '1000', 'the source has 1000 pages');

# A share: its copy has the source's map, every page all-visible.
my $c = sql(qq{SELECT test_bmap_share($s)});
adopt('c', 's', $c);
is(sql(q{SELECT all_visible FROM pg_visibility_map_summary('c')}), '1000',
	'the copy\'s pages are all-visible, as its map is the source\'s');

# Its first VACUUM reads every page, skipped as all-visible, for its free
# space; a second one reads none of them again.
cmp_ok(vacuum_reads('c'), '>=', 1000,
	'the copy\'s first VACUUM reads the pages it skips, for their free space');
cmp_ok(vacuum_reads('c'), '<', 100, 'and its next one skips them');
is(sql(q{SELECT all_visible FROM pg_visibility_map_summary('c')}), '1000',
	'reading changes no page: every page is still all-visible');
is(sql(q{SELECT count(*) FROM pg_check_visible('c')}), '0',
	'and rightly so');

fill('s');
fill('c');
is(pages('s'), '1000', 'inserts into the source fill its free space');
is(pages('c'), '1000', 'and those into the copy, after VACUUM, fill the copy\'s');

# A share whose copy then loses rows: VACUUM finds the space they leave
# and the space its other pages had all along.  (A row in every hundred
# pages: the pages between are many enough to be skipped.)
my $t = make_table('t');
my $d = sql(qq{SELECT test_bmap_share($t)});
adopt('d', 't', $d);
sql(q{DELETE FROM d WHERE a % 200 = 0});
sql(q{VACUUM d});
sql(q{INSERT INTO d SELECT -g, repeat('z', 3000) FROM generate_series(1, 10) g});
fill('d');
is(pages('d'), '1000',
	'after deletes and VACUUM, inserts into a copy fit its freed space and its pages\' own');

# Storage made from a captured root, without another relation's free
# space map, and with one.
my $root = sql(qq{SELECT test_bmap_root_capture($t)});
my $r = sql(qq{SELECT test_bmap_storage_from_root($root)});
adopt('r', 't', $r);
sql(q{VACUUM r});
fill('r');
is(pages('r'), '1000', 'storage made from a root finds its free space as it is vacuumed');
my $m = sql(qq{SELECT test_bmap_storage_from_root($root, $t)});
adopt('m', 't', $m);
sql(q{VACUUM m});
fill('m');
is(pages('m'), '1000', 'and so does one made with another relation\'s free space map');

# What VACUUM recorded holds after a restart.
sql(q{VACUUM (FREEZE) s});
my $e = sql(qq{SELECT test_bmap_share($s)});
adopt('e', 's', $e);
sql(q{VACUUM e});
$node->restart;
cmp_ok(vacuum_reads('e'), '<', 100,
	'after a restart, VACUUM no longer reads the skipped pages');

$node->stop;
done_testing();
