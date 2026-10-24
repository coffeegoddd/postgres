# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery of block-mapped relations whose logical blocks were given
# other physical blocks.  Map pages on disk can be newer or older than the
# record being replayed, so one physical block can be reached both by the
# records of the relation that let it go (r), through an older map page,
# and by those of the relation that has it now (s) from before it got it,
# through a newer one.  r's replay may find s's page there; s's replay
# mustn't write anything there.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# (the tables have no change logs: a log's pages, allocated as its table's
# rows are written, would take the freed blocks each scenario arranges for
# the tables.  A copy dropped frees the leaf of its map with the block it
# held, the leaf first in the store, and so first taken again: a table,
# fill, extended by a block, takes it.)

# $s_page: the logical block of s that gets r's old block (r's is 0).
sub scenario
{
	my ($name, $s_page) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->append_conf('postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
full_page_writes = off
});
	$node->start;
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };

	# Four rows to a page of s.  q's block, then r's, are allocated first.
	$sql->(q{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION pg_buffercache;
		CREATE SCHEMA cow VERSIONED;
		CREATE TABLE cow.fill (a int8);
		SELECT test_drop_change_log('cow.fill');
		INSERT INTO cow.fill VALUES (1);
		CREATE TABLE cow.q (a int8);
		SELECT test_drop_change_log('cow.q');
		-- (the change logs' storage, queued by their drops, let go of: their
		-- blocks free for the tables, as each scenario arranges)
		SELECT * FROM test_bmap_release_drain();
	});
	$sql->(q{INSERT INTO cow.q VALUES (1)}) if $s_page > 0;
	$sql->(q{
		CREATE TABLE cow.r (a int8, b int8);
		SELECT test_drop_change_log('cow.r');
		INSERT INTO cow.r SELECT g, g FROM generate_series(1, 150) g;
		CREATE TABLE cow.s (a int8, b char(1900));
		SELECT test_drop_change_log('cow.s');
		SELECT * FROM test_bmap_release_drain();
		CREATE TABLE churn (a int8, b text);
	});
	my $r = $sql->(q{SELECT pg_relation_filenode('cow.r')});
	my $s = $sql->(q{SELECT pg_relation_filenode('cow.s')});
	my $fill = $sql->(q{SELECT pg_relation_filenode('cow.fill')});
	my $r_block = $sql->(qq{SELECT phys FROM test_bmap_map($r) WHERE blkno = 0});
	is($sql->(q{SELECT max(ctid) FROM cow.r}), '(0,150)', "$name: r is one page");

	# s's map page is older than the redo point, and s's page $s_page is
	# made after it by its insert's record (a page of s before it is full;
	# s's first page, emptied, has its entry and no rows).
	if ($s_page > 0)
	{
		$sql->(q{INSERT INTO cow.s SELECT g, 'x' FROM generate_series(1, 4) g});
	}
	else
	{
		$sql->(q{INSERT INTO cow.s VALUES (1, 'x')});
		$sql->(q{DELETE FROM cow.s});
		$sql->(q{VACUUM cow.s});
	}
	$sql->(q{CHECKPOINT});

	# Replay makes s's page and evicts it; then r's page is changed by a
	# record without an image.
	$sql->(q{INSERT INTO cow.s VALUES (5, 'x')});
	$sql->(q{INSERT INTO churn SELECT g, repeat('x', 500) FROM generate_series(1, 5000) g});
	$sql->(q{UPDATE cow.r SET b = -b WHERE a = 150});

	# r lets its block go: shared with a copy, r is emptied, truncated and
	# grown again.  The copy, dropped, frees it.
	my $rcopy = $sql->(qq{SELECT test_bmap_share($r)});
	$sql->(q{DELETE FROM cow.r});
	$sql->(q{VACUUM cow.r});
	$sql->(q{INSERT INTO cow.r VALUES (1, 1)});
	isnt($sql->(qq{SELECT phys FROM test_bmap_map($r) WHERE blkno = 0}), $r_block,
		"$name: r let its block go");

	# s does the same (q dropped first, if it holds a block): s's page
	# $s_page gets r's old block.  A truncation frees the blocks past the
	# length once done (#638), so s's emptied pages (copied to free blocks,
	# being shared) and its change log's give their blocks back, and s's
	# growth takes blocks for its log too: s's pages before $s_page, and
	# its log's, take theirs first, and r's copy is dropped only then.
	my $scopy = $sql->(qq{SELECT test_bmap_share($s)});
	$sql->(q{DROP TABLE cow.q});
	$sql->(q{SELECT * FROM test_bmap_release_drain()});
	$sql->(q{DELETE FROM cow.s});
	$sql->(q{VACUUM cow.s});
	my $first = 4 * $s_page;
	$sql->(qq{INSERT INTO cow.s SELECT g, 'y' FROM generate_series(1, $first) g})
	  if $first > 0;
	$sql->(qq{SELECT test_bmap_unlink($rcopy)});
	$sql->(qq{SELECT test_bmap_extend($fill, 1)});
	$sql->(qq{INSERT INTO cow.s VALUES ($first + 1, 'y')});
	is($sql->(qq{SELECT phys FROM test_bmap_map($s) WHERE blkno = $s_page}), $r_block,
		"$name: s's page $s_page has r's old block");

	# s's map page reaches disk, r's doesn't; replay has few buffers.
	$sql->(q{SELECT pg_buffercache_evict_relation('cow.s')});
	$node->append_conf('postgresql.conf', 'shared_buffers = 16');
	$node->stop('immediate');
	$node->start;

	is($sql->(q{SELECT count(*) FROM cow.s WHERE b = 'y'}), 4 * $s_page + 1,
		"$name: s recovered");
	is($sql->(q{SELECT a, b FROM cow.r}), '1|1', "$name: r recovered");

	$sql->(qq{SELECT test_bmap_unlink($scopy)});
	$node->stop;
}

# r's replay finds s's page, which fails verification as r's block.
scenario('other_block', 1);

# r's replay finds a page of the same logical block: had s's replay written
# its page from before it got the block there, r's records would apply to it.
scenario('same_block', 0);

# A shared page copied before it changes, to a block r had: s's records
# from before the copy, replayed through s's newer map page, would find r's
# page of the same logical block there (the copy's image comes later).
{
	my $node = PostgreSQL::Test::Cluster->new('copied');
	$node->init;
	$node->append_conf('postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
full_page_writes = off
});
	$node->start;
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };

	$sql->(q{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION pg_buffercache;
		CREATE SCHEMA cow VERSIONED;
		CREATE TABLE cow.r (a int8, b int8);
		SELECT test_drop_change_log('cow.r');
		SELECT * FROM test_bmap_release_drain();
		INSERT INTO cow.r SELECT g, g FROM generate_series(1, 150) g;
		CREATE TABLE cow.s (a int8, b int8);
		SELECT test_drop_change_log('cow.s');
		SELECT * FROM test_bmap_release_drain();
		INSERT INTO cow.s SELECT g, g FROM generate_series(1, 5) g;
	});
	my $r = $sql->(q{SELECT pg_relation_filenode('cow.r')});
	my $s = $sql->(q{SELECT pg_relation_filenode('cow.s')});
	my $r_block = $sql->(qq{SELECT phys FROM test_bmap_map($r) WHERE blkno = 0});
	$sql->(q{CHECKPOINT});

	# A change of s's page without an image, before it is shared.
	$sql->(q{UPDATE cow.s SET b = -b WHERE a = 5});

	# r is dropped, freeing its block; s, shared, copies its page to it.
	$sql->(q{DROP TABLE cow.r});
	$sql->(q{SELECT * FROM test_bmap_release_drain()});
	my $copy = $sql->(qq{SELECT test_bmap_share($s)});
	my $copy_digest = $sql->(qq{SELECT test_bmap_rows_digest($copy)});
	$sql->(q{UPDATE cow.s SET b = 0 WHERE a = 1});
	is($sql->(qq{SELECT phys FROM test_bmap_map($s) WHERE blkno = 0}), $r_block,
		'copied: s\'s page went to r\'s old block');

	# s's map page reaches disk, its page doesn't.
	$sql->(qq{SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache
		WHERE relfilenode = $s AND relforknumber = 4});
	$node->stop('immediate');
	$node->start;

	is($sql->(q{SELECT string_agg(a || ':' || b, ',' ORDER BY a) FROM cow.s}),
		'1:0,2:2,3:3,4:4,5:-5', 'copied: s recovered');
	is($sql->(qq{SELECT test_bmap_rows_digest($copy)}), $copy_digest,
		'copied: the copy recovered');

	$sql->(qq{SELECT test_bmap_unlink($copy)});
	$node->stop;
}

# The other way: r copies its page away from a block it shared, which s
# gets when the copy is dropped.  r's records from before, replayed through
# r's older map page, find s's page of another logical block there: read as
# invalid, until replaying r's copy forgets it.
{
	my $node = PostgreSQL::Test::Cluster->new('copied_away');
	$node->init(allows_streaming => 1);
	$node->append_conf('postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
full_page_writes = off
});
	$node->start;
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };

	$sql->(q{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION pg_buffercache;
		CREATE SCHEMA cow VERSIONED;
		CREATE TABLE cow.fill (a int8);
		SELECT test_drop_change_log('cow.fill');
		SELECT * FROM test_bmap_release_drain();
		INSERT INTO cow.fill VALUES (1);
		CREATE TABLE cow.r (a int8, b int8);
		SELECT test_drop_change_log('cow.r');
		SELECT * FROM test_bmap_release_drain();
		INSERT INTO cow.r SELECT g, g FROM generate_series(1, 150) g;
		CREATE TABLE cow.s (a int8, b char(1900));
		SELECT test_drop_change_log('cow.s');
		SELECT * FROM test_bmap_release_drain();
		INSERT INTO cow.s SELECT g, 'x' FROM generate_series(1, 4) g;
	});
	my $fill = $sql->(q{SELECT pg_relation_filenode('cow.fill')});
	my $r = $sql->(q{SELECT pg_relation_filenode('cow.r')});
	my $s = $sql->(q{SELECT pg_relation_filenode('cow.s')});
	my $r_block = $sql->(qq{SELECT phys FROM test_bmap_map($r) WHERE blkno = 0});
	$sql->(q{CHECKPOINT});

	# A change of r's page without an image, before it is shared.
	$sql->(q{UPDATE cow.r SET b = -b WHERE a = 150});

	# r, shared, copies its page away; the copy, dropped, frees the block,
	# which s's second page gets.
	my $copy = $sql->(qq{SELECT test_bmap_share($r)});
	$sql->(q{UPDATE cow.r SET b = 0 WHERE a = 1});
	$sql->(qq{SELECT test_bmap_unlink($copy)});
	$sql->(qq{SELECT test_bmap_extend($fill, 1)});
	$sql->(q{INSERT INTO cow.s VALUES (5, 'y')});
	is($sql->(qq{SELECT phys FROM test_bmap_map($s) WHERE blkno = 1}), $r_block,
		'copied_away: s\'s second page has r\'s old block');

	# s's pages reach disk, r's map page doesn't.  Recovery as a standby
	# checks for invalid pages when it reaches consistency.
	$sql->(q{SELECT pg_buffercache_evict_relation('cow.s')});
	$node->stop('immediate');
	$node->set_standby_mode;
	$node->start;
	$node->promote;
	$node->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()');

	is($sql->(q{SELECT count(*), sum(b) FROM cow.r}), '150|11024',
		'copied_away: r recovered');
	is($sql->(q{SELECT count(*) FROM cow.s}), '5', 'copied_away: s recovered');
	$node->stop;
}

done_testing();
