# Copyright (c) 2026, PostgreSQL Global Development Group

# A share logs images of its source's own blocks that a recovery may need
# (PRESERVE records), the visibility map's among them now that the map is
# in the relation's tree.  A map page keeps its bits where a standard page
# has its hole, so its image must be logged whole: logged as a standard
# page, its bits were dropped, and a standby restored the map empty.  The
# standby's maps are checked after the share and after changes that copy
# map pages, after a crash of the standby, and once it is promoted, where
# the copy's VACUUM also finds the free space of its pages.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('share_vm_images');
$node->init(allows_streaming => 1);
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
	CREATE EXTENSION pg_walinspect;
	CREATE EXTENSION pg_visibility;
});

# A standby from a backup taken before the tables are made.
$node->backup('before');
my $standby = PostgreSQL::Test::Cluster->new('share_vm_images_standby');
$standby->init_from_backup($node, 'before', has_streaming => 1);
$standby->start;

# A table of 2000 pages of two 3000-byte rows each, about 2 kB free on
# every page, all-visible and all-frozen: made after the redo pointer, so
# that a share logs images of its pages, its map's too.
sql(q{
	CREATE TABLE src (a int, b text);
	ALTER TABLE src ALTER COLUMN b SET STORAGE PLAIN;
	INSERT INTO src SELECT g, repeat('x', 3000) FROM generate_series(1, 4000) g;
	VACUUM (FREEZE) src;
});
my $src = sql(q{SELECT pg_relation_filenode('src')});
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
my $copy = sql(qq{SELECT test_bmap_share($src)});
sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
cmp_ok(sql(qq{
	SELECT count(*) FROM pg_get_wal_block_info('$start', pg_current_wal_insert_lsn(), false)
	WHERE relfilenode = $src AND relforknumber = 2 AND block_fpi_length > 0}),
	'>', 0, 'the share logs images of the source\'s map pages');

# The copy made a table of its own, so the catalogs can reach it.
sql(qq{
	CREATE TABLE cp (LIKE src);
	ALTER TABLE cp ALTER COLUMN b SET STORAGE PLAIN;
	UPDATE pg_class SET relfilenode = $copy,
		relfrozenxid = (SELECT relfrozenxid FROM pg_class WHERE relname = 'src'),
		relminmxid = (SELECT relminmxid FROM pg_class WHERE relname = 'src')
		WHERE relname = 'cp';
});

# Changes of either relation after the share clear bits, copying the
# map pages that hold them.
sql(q{DELETE FROM cp WHERE a % 400 = 0; DELETE FROM src WHERE a % 400 = 1});

sub vm
{
	my ($n, $t) = @_;
	return $n->safe_psql('postgres',
		qq{SELECT string_agg(all_visible::int::text || all_frozen::int::text, '' ORDER BY blkno)
		   FROM pg_visibility_map('$t')});
}

sub compare_maps
{
	my ($n, $what) = @_;

	foreach my $t ('src', 'cp')
	{
		is(vm($n, $t), vm($node, $t), "$what: the map of $t is the primary's");
		is($n->safe_psql('postgres', qq{SELECT count(*) FROM pg_check_visible('$t')}),
			'0', "$what: and right there");
	}
	is($n->safe_psql('postgres', q{SELECT all_visible FROM pg_visibility_map_summary('cp')}),
		'1990', "$what: the copy's pages are all-visible but those changed");
	is($n->safe_psql('postgres', q{SELECT count(*), sum(a) FROM cp}),
		sql(q{SELECT count(*), sum(a) FROM cp}), "$what: the copy's rows");
}

$node->wait_for_replay_catchup($standby);
compare_maps($standby, 'the standby');

# A crash of the standby, its buffers lost: the maps are read from its disk
# and the WAL replayed since its restart point.
$standby->stop('immediate');
$standby->start;
$node->wait_for_replay_catchup($standby);
compare_maps($standby, 'the standby after a crash');

# Promoted: the maps are as they were, and the copy's VACUUM finds the free
# space of the pages it skips as all-visible, as it would on the primary.
$standby->promote;
compare_maps($standby, 'the promoted standby');
my $pages = $standby->safe_psql('postgres', q{SELECT pg_relation_size('cp') / 8192});
$standby->safe_psql('postgres', q{VACUUM cp});
$standby->safe_psql('postgres',
	q{INSERT INTO cp SELECT -g, repeat('y', 1500) FROM generate_series(1, 1000) g});
is($standby->safe_psql('postgres', q{SELECT pg_relation_size('cp') / 8192}), $pages,
	'the promoted standby\'s VACUUM of the copy finds its free space');
is($standby->safe_psql('postgres', q{SELECT count(*) FROM pg_check_visible('cp')}),
	'0', 'and the copy\'s map is right after the inserts');

$standby->stop;
$node->stop;
done_testing();
