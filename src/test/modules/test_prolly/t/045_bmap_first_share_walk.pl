# Copyright (c) 2026, PostgreSQL Global Development Group

# A share first walks the source's own blocks, syncing those it got before
# the checkpoint last completed and logging images of the others.  The walk
# costs what the source changed since the checkpoint, not its size: a
# leaf last changed before the checkpoint got all its blocks before it, so
# it is counted whole, its entries not looked at; and a block set in the
# map's epoch is the map's own, so its count isn't asked.  A first share
# of a table never shared, checkpointed, looks at no entry and asks no
# count.  The copies it makes still survive a crash, as do those of a
# table changed since the checkpoint, whose changed blocks are logged.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('first_share_walk');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
block_map_standby_hold_max_size = 0
checkpoint_timeout = 1h
max_wal_size = 1GB
bgwriter_lru_maxpages = 0
log_min_messages = debug1
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_walinspect;
});

sub sql { return $node->safe_psql('postgres', $_[0]); }

# A table of about 'pages' pages (fewer, full, if 'fillfactor' is 100),
# rows frozen.
sub make_table
{
	my ($name, $pages, $fillfactor) = @_;

	$fillfactor //= 50;
	sql(qq{
		CREATE TABLE $name (a int, b text) WITH (fillfactor = $fillfactor);
		INSERT INTO $name SELECT g, repeat('x', 200)
			FROM generate_series(1, $pages * 17) g;
		VACUUM (FREEZE) $name;
	});
	return sql(qq{SELECT pg_relation_filenode('$name')});
}

# Shares 'filenode' and returns the copy, with what the share's walk
# reported: own, synced, leaves counted whole, entries looked at, counts
# asked.
sub share
{
	my ($name, $filenode) = @_;
	my $path = sql(qq{SELECT pg_relation_filepath('$name')});
	my $offset = -s $node->logfile;
	my $copy = sql(qq{SELECT test_bmap_share($filenode)});
	my $log = slurp_file($node->logfile, $offset);

	$log =~ /a share of \Q$path\E found (\d+) own blocks, (\d+) of them synced: (\d+) leaves counted whole, (\d+) entries looked at, (\d+) counts asked/
	  or die "no walk reported for the share of $name";
	return ($copy, { own => $1, synced => $2, whole => $3, entries => $4, asked => $5 });
}

# Full-page images of relation file 'filenode''s main fork from 'start' on.
sub images_since
{
	my ($start, $filenode) = @_;

	sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
	return sql(qq{
		SELECT count(*) FROM pg_get_wal_block_info('$start', pg_current_wal_insert_lsn(), false)
		WHERE relfilenode = $filenode AND relforknumber = 0 AND block_fpi_length > 0});
}

# A table of several leaves (a leaf maps 500-odd blocks), checkpointed.
my $big = make_table('big', 1600);
my $big_pages = sql(q{SELECT pg_relation_size('big') / 8192});
sql(q{CHECKPOINT});
my $big_digest = sql(qq{SELECT test_bmap_rows_digest($big)});
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
my ($big_copy, $walk) = share('big', $big);
cmp_ok($walk->{whole}, '>=', 3, 'a checkpointed table\'s leaves are counted whole');
is($walk->{entries}, 0, 'no entry of theirs is looked at');
is($walk->{asked}, 0, 'and no count asked: the table was never shared');
cmp_ok($walk->{synced}, '>=', $big_pages, 'every block is synced');
is(images_since($start, $big), 0, 'none is logged');

# Changed since the checkpoint: a checkpointed table, its pages full, and a
# change in place of one of them, which keeps the block it got before the
# checkpoint, and rows that extend it, by blocks it gets since.  The last
# leaf, which got them, is looked into, and they are logged; the others
# are counted whole.  Then a table made since.
my $mixed = make_table('mixed', 1600, 100);
sql(q{CHECKPOINT});
sql(q{UPDATE mixed SET b = 'y' WHERE a = 1});
sql(q{INSERT INTO mixed SELECT g, repeat('z', 200) FROM generate_series(1, 340) g});
my $mixed_digest = sql(qq{SELECT test_bmap_rows_digest($mixed)});
$start = sql(q{SELECT pg_current_wal_insert_lsn()});
my ($mixed_copy, $mwalk) = share('mixed', $mixed);
cmp_ok($mwalk->{whole}, '>=', 2, 'the leaves unchanged since the checkpoint are counted whole');
cmp_ok($mwalk->{entries}, '>=', 1, 'the leaf that got blocks since is looked into');
cmp_ok(images_since($start, $mixed), '>=', 1, 'and those blocks are logged');

my $recent = make_table('recent', 200);
my $recent_digest = sql(qq{SELECT test_bmap_rows_digest($recent)});
$start = sql(q{SELECT pg_current_wal_insert_lsn()});
my ($recent_copy, $rwalk) = share('recent', $recent);
is($rwalk->{whole}, 0, 'a table made since the checkpoint has no leaf counted whole');
cmp_ok(images_since($start, $recent), '>=',
	sql(q{SELECT pg_relation_size('recent') / 8192}),
	'every block it got since is logged');

# A crash: recovery begins at the checkpoint, before every share, and each
# copy reads what its source held, as does each source.
$node->stop('immediate');
$node->start;
is(sql(qq{SELECT test_bmap_rows_digest($big_copy)}), $big_digest,
	'the copy of a checkpointed table survives a crash');
is(sql(qq{SELECT test_bmap_rows_digest($big)}), $big_digest, 'as does its source');
is(sql(qq{SELECT test_bmap_rows_digest($mixed_copy)}), $mixed_digest,
	'the copy of a table changed since the checkpoint survives it');
is(sql(qq{SELECT test_bmap_rows_digest($mixed)}), $mixed_digest, 'as does its source');
is(sql(qq{SELECT test_bmap_rows_digest($recent_copy)}), $recent_digest,
	'the copy of a table made since the checkpoint survives it');

done_testing();
