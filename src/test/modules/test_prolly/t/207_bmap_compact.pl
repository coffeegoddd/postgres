# Copyright (c) 2026, PostgreSQL Global Development Group

# A page sweep moves the blocks past the page store's allocated count down
# into the holes freeing left (a table's blocks among a dropped one's), and
# shortens the store past them: its file is then as long as what it holds.
# The moves are logged as copies before a change are: a standby replays
# them, as does crash recovery, every page checked against the primary's
# (wal_consistency_checking).  Blocks another relation shares stay, and a
# crash, or an error, between a move and the freeing of its old block
# leaves that block for the next sweep.  At wal_level minimal, a relation
# its transaction hasn't committed (its data skipping WAL) isn't moved.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('compact');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
wal_consistency_checking = 'all'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('compact_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $dboid = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
sub store_blocks
{
	my ($n) = @_;
	return (-s $n->data_dir . "/base/$dboid/9421") / 8192;
}
my $sweep = q{SELECT freed || ' ' || truncated FROM prolly_page_sweep()};
my $moved = q{SELECT pg_stat_force_next_flush();
	SELECT value FROM pg_stat_prolly WHERE name = 'bmap_blocks_moved'};

# Two tables extended in turn, so their blocks alternate in the store;
# the first has an index, whose blocks are among them too.
sub interleave
{
	my ($keep, $gone, $rounds) = @_;
	sql(qq{CREATE TABLE $keep (a int PRIMARY KEY, b text) WITH (fillfactor = 10);
		CREATE TABLE $gone (a int, b text) WITH (fillfactor = 10)});
	sql(qq{DO \$\$
		BEGIN
			FOR i IN 1 .. $rounds LOOP
				INSERT INTO $keep SELECT g, repeat('k', 300)
					FROM generate_series(i * 10, i * 10 + 9) g;
				INSERT INTO $gone SELECT g, repeat('g', 300)
					FROM generate_series(i * 10, i * 10 + 9) g;
			END LOOP;
		END \$\$});
}
my $digest = q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM keep};

interleave('keep', 'gone', 400);
my $want = sql($digest);
sql('DROP TABLE gone');
sql($sweep);
my $before = sql($moved);
is(sql('SELECT test_bmap_free_blocks()') > 0 ? 'holes' : 'none', 'holes',
	'freeing the dropped table\'s blocks leaves holes a sweep doesn\'t shorten past');

# Holes are made only once the drop's blocks are freed; a second sweep
# moves the kept table's blocks down into them.
interleave('keep2', 'gone2', 200);
my $want2 = q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM keep2};
my $want2v = sql($want2);
sql('DROP TABLE gone2');
my $file_before = store_blocks($node);
my ($freed, $truncated) = split / /, sql($sweep);
cmp_ok(sql($moved) - $before, '>', 0, 'a sweep moves blocks down');
cmp_ok($truncated, '>', 0, 'and shortens the store past them');
is(sql('SELECT test_bmap_free_blocks()'), '0', 'no free block is left in it');
cmp_ok(store_blocks($node), '<', $file_before, 'its file is shorter');
is(sql($digest), $want, 'the moved tables keep their rows');
is(sql($want2), $want2v, 'both of them');
is(sql(q{SET enable_seqscan = off; SELECT count(*) FROM keep WHERE a BETWEEN 100 AND 2000}),
	'1901', 'and their indexes find them');

$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $digest), $want, 'a standby replays the moves');
is(store_blocks($standby), store_blocks($node), 'and shortens its store as much');

# A crash: recovery replays the moves (from a checkpoint before them).
$node->stop('immediate');
$node->start;
is(sql($digest), $want, 'crash recovery replays the moves');
is(sql($want2), $want2v, 'of both tables');

# A copy shares a table's blocks: they stay where they are.
interleave('shared', 'gone3', 100);
my $srel = sql(q{SELECT pg_relation_filenode('shared')});
my $copy = sql(qq{SELECT test_bmap_share($srel)});
my $map = qq{SELECT string_agg(blkno || ':' || phys, ',' ORDER BY blkno) FROM test_bmap_map($srel)};
my $shared_map = sql($map);
sql('DROP TABLE gone3');
sql($sweep);
is(sql($map), $shared_map, 'a sweep moves no block another relation shares');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), sql(qq{SELECT test_bmap_rows_digest($srel)}),
	'the copy reads the same rows');

# An error between the moves and the freeing of the old blocks: they stay
# allocated, held by nothing, and the next sweep frees them.
interleave('keep4', 'gone4', 100);
my $want4 = q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM keep4};
my $want4v = sql($want4);
sql('DROP TABLE gone4');
sql(q{SELECT injection_points_attach('bmap-compact-moved', 'error')});
sql($sweep);
sql(q{SELECT injection_points_detach('bmap-compact-moved')});
cmp_ok(sql('SELECT test_bmap_free_blocks()'), '>', 0,
	'an error before the old blocks are freed leaves them');
is(sql($want4), $want4v, 'the moved table has its rows');
($freed, $truncated) = split / /, sql($sweep);
cmp_ok($freed, '>', 0, 'the next sweep frees what the moves left');
is(sql('SELECT test_bmap_free_blocks()'), '0', 'and moves down into them');
is(sql($want4), $want4v, 'the table still has its rows');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $want4), $want4v, 'on the standby too');

$standby->stop;
$node->stop;

# wal_level minimal: a table made in a transaction not yet committed skips
# WAL, and isn't moved; its rows survive the commit and a crash after it.
my $min = PostgreSQL::Test::Cluster->new('compact_minimal');
$min->init;
$min->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
wal_level = minimal
max_wal_senders = 0
wal_skip_threshold = 0
});
$min->start;
sub msql { return $min->safe_psql('postgres', $_[0]); }
msql('CREATE EXTENSION test_prolly');
msql(q{CREATE TABLE gone (a int, b text) WITH (fillfactor = 10);
	INSERT INTO gone SELECT g, repeat('g', 300) FROM generate_series(1, 2000) g});
my $open = $min->background_psql('postgres');
$open->query_safe(q{BEGIN;
	CREATE TABLE fresh (a int, b text) WITH (fillfactor = 10);
	INSERT INTO fresh SELECT g, repeat('f', 300) FROM generate_series(1, 2000) g});
my $fresh = $open->query_safe(q{SELECT pg_relation_filenode('fresh')});
my $fresh_map = qq{SELECT string_agg(blkno || ':' || phys, ',' ORDER BY blkno) FROM test_bmap_map($fresh)};
my $fresh_before = msql($fresh_map);
msql('DROP TABLE gone');
msql('SELECT * FROM prolly_page_sweep()');
is(msql($fresh_map), $fresh_before, 'a table whose data skips WAL is not moved');
$open->query_safe(q{UPDATE fresh SET b = repeat('u', 300) WHERE a % 2 = 0; COMMIT});
$open->quit;
my $fresh_digest = q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM fresh};
my $fresh_want = msql($fresh_digest);
$min->stop('immediate');
$min->start;
is(msql($fresh_digest), $fresh_want, 'and has its rows after a crash');
$min->stop;

done_testing();
