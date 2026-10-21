# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery where a full-page image of a map page is older than the
# map page on disk.  Redo skips a block's records from before the WAL
# position since which its entry has its physical block, as the map page
# says, and doesn't write pages made by them: the record that gave it that
# block carries the page's image.  Read from the newer map page on disk, a
# block remapped late in the WAL has its records skipped from the start of
# the replay; an image of the map page restored part way through gives the
# entry its older position again, and must not make redo apply the block's
# later records to a page missing the ones skipped before, nor write a page
# from before the remap to the physical block the image maps, which another
# relation shares by then.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub new_node
{
	my ($name) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->append_conf('postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
full_page_writes = on
});
	$node->start;
	$node->safe_psql('postgres', q{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION pg_buffercache;
		CREATE EXTENSION pg_walinspect;
		CREATE SCHEMA cow VERSIONED;
		CREATE TABLE cow.w (a int8);
		INSERT INTO cow.w VALUES (1);
		CREATE TABLE cow.s (a int8, b int8, c char(200)) WITH (fillfactor = 50);
		INSERT INTO cow.s SELECT g, 0, 'x' FROM generate_series(1, 40) g;
		CREATE TABLE churn (a int8, b text);
	});
	return $node;
}

# Is there an image of map page 1 of relation file 'rel' in the WAL between
# 'from' and 'to'?
sub map_image
{
	my ($node, $rel, $from, $to) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT count(*) > 0 FROM pg_get_wal_block_info('$from', '$to')
		WHERE relfilenode = $rel AND relforknumber = 4
		AND relblocknumber = 1 AND block_fpi_length > 0});
}

# The beginning of both scenarios below.  s has a few pages, all of them on
# map page 1, with room on each for updates that stay on the page.  After a
# checkpoint, block 0 changes twice, the first change logging its image.  s
# is shared, and its last page copied before it changes: the map page's
# first change since the checkpoint logs its image.  The copy copies its
# block 0 away, so s's block 0 is s's alone again and changes in place.
# Returns s, the copy, and block 0's physical block.
sub prepare
{
	my ($node, $name) = @_;
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };
	my $s = $sql->(q{SELECT pg_relation_filenode('cow.s')});

	is($sql->(q{SELECT (max(ctid)::text::point)[0] FROM cow.s}), '2',
		"$name: s has three pages");
	$sql->(q{CHECKPOINT});
	$sql->(q{UPDATE cow.s SET b = 1 WHERE a = 1});
	$sql->(q{UPDATE cow.s SET b = 2 WHERE a = 1});
	my $before = $sql->(q{SELECT pg_current_wal_insert_lsn()});
	my $copy = $sql->(qq{SELECT test_bmap_share($s)});
	$sql->(q{UPDATE cow.s SET b = 1 WHERE a = 40});
	$sql->(qq{SELECT test_bmap_write($copy, 0, 1)});
	my $phys = $sql->(qq{SELECT phys FROM test_bmap_map($s) WHERE blkno = 0});
	is(map_image($node, $s, $before, $sql->(q{SELECT pg_current_wal_insert_lsn()})),
		't', "$name: the WAL has an image of the map page after block 0's changes");
	$sql->(q{UPDATE cow.s SET b = 3 WHERE a = 1});
	is($sql->(qq{SELECT phys FROM test_bmap_map($s) WHERE blkno = 0}),
		$phys, "$name: block 0 changed in place");
	return ($s, $copy, $phys);
}

# Shared again, s's block 0 is copied to a new block, late in the WAL: the
# copy keeps the old one.  Its map page as of then reaches disk, and the
# server crashes (and restarts with 'conf').  Returns the copy, and its
# rows' digest before the crash.
sub remap_and_crash
{
	my ($node, $name, $s, $phys, $b, $conf) = @_;
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };

	my $copy = $sql->(qq{SELECT test_bmap_share($s)});
	my $digest = $sql->(qq{SELECT test_bmap_rows_digest($copy)});
	$sql->(qq{UPDATE cow.s SET b = $b WHERE a = 1});
	isnt($sql->(qq{SELECT phys FROM test_bmap_map($s) WHERE blkno = 0}),
		$phys, "$name: block 0 went to a new block");
	is($sql->(qq{SELECT phys FROM test_bmap_map($copy) WHERE blkno = 0}),
		$phys, "$name: the copy keeps the old block");
	$sql->(qq{SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache
		WHERE relfilenode = $s AND relforknumber = 4});
	$node->stop('immediate');
	$node->append_conf('postgresql.conf', $conf) if $conf;
	ok($node->start(fail_ok => 1), "$name: recovery completes");
	return ($copy, $digest);
}

# The replay skips block 0's second change (the map page on disk is newer),
# and then must skip the change in place, which needs it, though the map
# page's image is in place by then.
{
	my $name = 'applied';
	my $node = new_node($name);
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };
	my ($s, $copy, $phys) = prepare($node, $name);
	my ($copy2, $digest2) = remap_and_crash($node, $name, $s, $phys, 4);

	is($sql->(q{SELECT b FROM cow.s WHERE a = 1}), '4',
		"$name: block 0 recovered");
	is($sql->(q{SELECT count(*), sum(b) FROM cow.s}), '40|5',
		"$name: s recovered");
	is($sql->(qq{SELECT test_bmap_rows_digest($copy2)}), $digest2,
		"$name: the second copy recovered");
	$node->stop;
}

# A checkpoint that began but didn't end makes block 0's next change log an
# image again, which the replay restores while the map page's image is in
# place.  Replay evicts that page, from before the remap (its later change
# skipped), and mustn't write it to the old block, the second copy's.
SKIP:
{
	skip 'Injection points not supported by this build', 1
	  if $ENV{enable_injection_points} ne 'yes';

	my $name = 'written';
	my $node = new_node($name);
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };
	$sql->(q{CREATE EXTENSION injection_points});
	my ($s, $copy, $phys) = prepare($node, $name);

	# w, made first, is the first block-mapped relation a checkpoint
	# writes: it stops there.
	$sql->(q{UPDATE cow.w SET a = 2});
	$sql->(q{SELECT injection_points_attach('bmap-write', 'wait')});
	my $c = $node->background_psql('postgres', on_error_stop => 0);
	$c->query_until(qr/started/, q{
		\echo started
		CHECKPOINT;
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0
		FROM pg_stat_activity
		WHERE backend_type = 'checkpointer' AND wait_event = 'bmap-write'})
	  or die 'the checkpoint did not stop';
	$sql->(q{SELECT injection_points_detach('bmap-write')});

	my $before = $sql->(q{SELECT pg_current_wal_insert_lsn()});
	$sql->(q{UPDATE cow.s SET b = 4 WHERE a = 1});
	is( $sql->(qq{SELECT count(*) > 0
			FROM pg_get_wal_block_info('$before', pg_current_wal_insert_lsn())
			WHERE relfilenode = $s AND relforknumber = 0
			AND relblocknumber = 0 AND block_fpi_length > 0}),
		't', "$name: block 0's change logs its image again");
	$sql->(q{UPDATE cow.s SET b = 5 WHERE a = 1});
	$sql->(q{INSERT INTO churn SELECT g, repeat('x', 500)
		FROM generate_series(1, 5000) g});
	my ($copy2, $digest2) =
	  remap_and_crash($node, $name, $s, $phys, 6, 'shared_buffers = 16');

	is($sql->(q{SELECT b FROM cow.s WHERE a = 1}), '6',
		"$name: block 0 recovered");
	is($sql->(q{SELECT count(*), sum(b) FROM cow.s}), '40|7',
		"$name: s recovered");
	is($sql->(qq{SELECT test_bmap_rows_digest($copy2)}), $digest2,
		"$name: the second copy recovered");
	$node->stop;
}

# More blocks ahead of the replay than recovery can remember: it stops
# rather than guess, and completes with room for them.
{
	my $node = new_node('too_many');
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };

	$sql->(q{
		CREATE TABLE cow.t (a int8, b int8, c char(1000)) WITH (fillfactor = 50);
		INSERT INTO cow.t SELECT g, 0, 'x' FROM generate_series(1, 300) g;
	});
	my $t = $sql->(q{SELECT pg_relation_filenode('cow.t')});
	cmp_ok($sql->(q{SELECT pg_relation_size('cow.t') / 8192}), '>', 70,
		'too_many: t has more pages than recovery is let remember');
	$sql->(q{CHECKPOINT});

	# Every page is copied, the first copy logging the map page's image.
	my $copy = $sql->(qq{SELECT test_bmap_share($t)});
	my $digest = $sql->(qq{SELECT test_bmap_rows_digest($copy)});
	$sql->(q{UPDATE cow.t SET b = 1});
	$sql->(qq{SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache
		WHERE relfilenode = $t AND relforknumber = 4});
	$node->stop('immediate');

	$node->append_conf('postgresql.conf', 'block_map_replay_entries = 64');
	ok(!$node->start(fail_ok => 1), 'too_many: recovery stops');
	like(
		slurp_file($node->logfile),
		qr/FATAL:  recovery needs to remember more remapped blocks of block-mapped relations than "block_map_replay_entries" \(64\) holds/,
		'too_many: for want of room');

	$node->append_conf('postgresql.conf', 'block_map_replay_entries = 1024');
	ok($node->start(fail_ok => 1), 'too_many: recovery completes with room');
	is($sql->(q{SELECT count(*), sum(b) FROM cow.t}), '300|300',
		'too_many: t recovered');
	is($sql->(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
		'too_many: the copy recovered');
	$node->stop;
}

done_testing();
