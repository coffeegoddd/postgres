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
		CREATE TABLE cow.q (a int8);
	});
	$sql->(q{INSERT INTO cow.q VALUES (1)}) if $s_page > 0;
	$sql->(q{
		CREATE TABLE cow.r (a int8, b int8);
		INSERT INTO cow.r SELECT g, g FROM generate_series(1, 150) g;
		CREATE TABLE cow.s (a int8, b char(1900));
		CREATE TABLE churn (a int8, b text);
	});
	my $r = $sql->(q{SELECT pg_relation_filenode('cow.r')});
	my $s = $sql->(q{SELECT pg_relation_filenode('cow.s')});
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
	$sql->(qq{SELECT test_bmap_unlink($rcopy)});

	# s does the same (q dropped first, if it holds a block): s's page
	# $s_page gets r's old block.
	my $scopy = $sql->(qq{SELECT test_bmap_share($s)});
	$sql->(q{DROP TABLE cow.q});
	$sql->(q{DELETE FROM cow.s});
	$sql->(q{VACUUM cow.s});
	$sql->(qq{INSERT INTO cow.s SELECT g, 'y' FROM generate_series(1, 4 * $s_page + 1) g});
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

done_testing();
