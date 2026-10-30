# Copyright (c) 2026, PostgreSQL Global Development Group

# The largest relations work: a block map reaching the last block a
# relation can have, MaxBlockNumber (2^32 - 2), built sparsely.  A write
# past a block-mapped relation's end (as a hash index's bucket allocation
# writes the last block of a range, or a copy sets its destination's size)
# maps the block written alone, leaving a hole between, as md.c's file
# does: a hole reads as zeroes, is given a block when it first changes, and
# has no map nodes where nothing below them is mapped.  So the relation
# costs the blocks it writes, not one for each block below its end; mapping
# every block below 2^32 would be 32 TB of store and tens of gigabytes of
# map.  The test checks the store holds only the blocks written, reads and
# writes at both ends and in the middle, truncation back, crash recovery
# and a standby's replay, and that a scan and VACUUM of a table with holes
# allocate none of them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('sparse_map');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Run in a transaction that commits with an id, so that its records are
# flushed by its commit, as an immediate stop after must find them.
sub durable
{
	return $node->safe_psql('postgres',
		"BEGIN; SELECT txid_current() \\g /dev/null\n$_[0];\nCOMMIT;");
}

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int);
	INSERT INTO t VALUES (1);
	CHECKPOINT;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
my $dboid = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});

# A standby following from here replays everything below.
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('sparse_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

sub store_blocks
{
	return sql(qq{SELECT (pg_stat_file('base/$dboid/9421')).size / 8192});
}

sub mapped
{
	my ($n, $rel) = @_;
	$rel //= $t;
	return ($n // $node)->safe_psql('postgres',
		qq{SELECT string_agg(blkno::text, ',' ORDER BY blkno) FROM test_bmap_mapped($rel)});
}

# A write past the end maps the block written alone.  (Checked on a short
# extension first: a map without holes would map the 5000 blocks between,
# and could not reach the last block at all.)
durable(qq{SELECT test_bmap_extend_at($t, 5000)});
is(sql(q{SELECT pg_relation_size('t') / 8192}), '5001',
	'a write past the end extends the relation to the block written');
is(mapped(), '0,5000', 'only the blocks written are mapped')
  or BAIL_OUT('extension maps the blocks it skips');
is(sql(qq{SELECT test_bmap_read($t, 2500, true)}), 'zeroes',
	'a hole reads as zeroes through shared buffers');
is(sql(qq{SELECT test_bmap_read($t, 2501, false)}), 'zeroes',
	'and through the storage manager');

# The last block a relation can have.
my $before = store_blocks();
my $last = 4294967294;
my $mid = 3000000000;
durable(qq{SELECT test_bmap_extend_at($t, $last)});
is(sql(q{SELECT pg_relation_size('t') / 8192}), $last + 1,
	'the relation reaches the last block a relation can have');
is(mapped(), "0,5000,$last", 'with a block for the blocks written only');
cmp_ok(store_blocks() - $before, '<=', 8,
	'the store grew by the block and the nodes of its path alone');

durable(qq{SELECT test_bmap_write($t, $last, 7)});
is(sql(qq{SELECT test_bmap_read($t, $last, true)}), '7',
	'the last block is written and read');
is(sql(qq{SELECT test_bmap_read($t, $mid, true)}), 'zeroes',
	'a hole between the ends reads as zeroes');

# A hole given a block by its first change, as an access method's new
# page is; nothing is copied, and the map gains that entry alone.
durable(qq{SELECT test_bmap_init_block($t, $mid)});
durable(qq{SELECT test_bmap_write($t, $mid, 9)});
durable(qq{SELECT test_bmap_init_block($t, 3)});
durable(qq{SELECT test_bmap_write($t, 3, 4)});
is(mapped(), "0,3,5000,$mid,$last",
	'a hole that changes is given a block, alone');
is(sql(qq{SELECT test_bmap_read($t, $mid, true)}), '9',
	'and reads what was written');
is(sql(qq{SELECT test_bmap_read($t, 2, true)}), 'zeroes',
	'its neighbor is a hole still');
is(sql(q{SELECT a FROM t WHERE ctid = '(0,1)'}), '1',
	'the table reads its row still');
cmp_ok(store_blocks() - $before, '<=', 16, 'the store holds little more');

# Crash recovery replays it.
$node->stop('immediate');
$node->start;
is(sql(q{SELECT pg_relation_size('t') / 8192}), $last + 1,
	'crash recovery keeps the length');
is(mapped(), "0,3,5000,$mid,$last", 'and the map');
is( sql(
		qq{SELECT test_bmap_read($t, $last, true) || ' ' || test_bmap_read($t, $mid, true) || ' ' || test_bmap_read($t, 4000000000, true)}
	),
	'7 9 zeroes',
	'and the pages, holes reading as zeroes');

# So does a standby.
$node->wait_for_catchup($standby);
is($standby->safe_psql('postgres', q{SELECT pg_relation_size('t') / 8192}),
	$last + 1, 'a standby has the length');
is(mapped($standby), "0,3,5000,$mid,$last", 'and the map');
is( $standby->safe_psql('postgres',
		qq{SELECT test_bmap_read($t, $last, true) || ' ' || test_bmap_read($t, $mid, true) || ' ' || test_bmap_read($t, 1234567890, true)}
	),
	'7 9 zeroes',
	'and the pages');

# A share of the map shares its holes: one given a block in the copy (its
# path copied from the shared nodes) stays a hole in the source, a shared
# page changed in the source is copied, and a diff of the two, a sweep and
# a capture read only the nodes they have.
my $copy = sql(qq{SELECT test_bmap_share($t)});
is(mapped(undef, $copy), "0,3,5000,$mid,$last", 'a share has the holes too');
my $far = 4000000000;
sql(qq{SELECT test_bmap_init_block($copy, $far)});
sql(qq{SELECT test_bmap_write($copy, $far, 3)});
sql(qq{SELECT test_bmap_write($t, $mid, 11)});
is( sql(
		qq{SELECT test_bmap_read($t, $far, true) || ' ' || test_bmap_read($copy, $far, true) || ' ' || test_bmap_read($t, $mid, true) || ' ' || test_bmap_read($copy, $mid, true)}
	),
	'zeroes 3 11 9',
	'a hole filled in the copy stays one in the source');
is(sql(qq{SELECT string_agg(d::text, ',' ORDER BY d) FROM test_bmap_diff($t, $copy) d}),
	"$mid,$far", 'a diff finds the blocks that differ');
my $root = sql(qq{SELECT test_bmap_root_capture($t)});
is(sql(qq{SELECT test_bmap_root_held($root)}), $last + 1,
	'a root captured of the map holds its length');
is(sql(qq{SELECT count(*) FROM test_bmap_diff_root($t, $root)}), '0',
	'and differs from it in nothing');
sql(qq{SELECT test_bmap_root_release($root)});
sql(qq{SELECT test_bmap_unlink($copy)});
sql(q{SELECT * FROM test_bmap_sweep()});
is(mapped(), "0,3,5000,$mid,$last", 'a sweep keeps the map');
is(sql(qq{SELECT test_bmap_read($t, $mid, true)}), '11',
	'and its pages');
durable(qq{SELECT test_bmap_write($t, $mid, 9)});

# Truncated back, the blocks past the end are let go of; nothing walks the
# billions of blocks between them.
my $free = sql(q{SELECT test_bmap_free_blocks()});
durable(qq{SELECT test_bmap_truncate($t, 4, true)});
is(sql(q{SELECT pg_relation_size('t') / 8192}), '4',
	'truncated to four blocks');
is(mapped(), '0,3', 'the blocks past the end let go of');
is(sql(q{SELECT test_bmap_free_blocks()}) - $free, 3, 'and freed');
is(sql(qq{SELECT test_bmap_read($t, 3, true)}), '4',
	'the blocks kept read as they were');

# A truncation a crash interrupted leaves entries past the end; a write
# past them makes them a hole, which must read as zeroes, not as the
# pages they had.
durable(qq{SELECT test_bmap_extend_at($t, 10)});
durable(qq{SELECT test_bmap_write($t, 10, 5)});
durable(qq{SELECT test_bmap_truncate($t, 4, false)});
is(mapped(), '0,3,10', 'a truncation not done leaves its entries');
durable(qq{SELECT test_bmap_extend_at($t, 20)});
is(sql(qq{SELECT test_bmap_read($t, 10, true)}), 'zeroes',
	'a hole made over them reads as zeroes');
is(mapped(), '0,3,20', 'their blocks let go of');

$node->stop('immediate');
$node->start;
is(sql(q{SELECT pg_relation_size('t') / 8192}), '21',
	'crash recovery keeps the truncation and the extension');
is(mapped(), '0,3,20', 'and the map');
$node->wait_for_catchup($standby);
is(mapped($standby), '0,3,20', 'as does the standby');
# (read from the storage: the truncation above, made below the catalogs,
# logged no truncation of the standby's buffers, as a table's does)
is($standby->safe_psql('postgres', qq{SELECT test_bmap_read($t, 10, false)}),
	'zeroes', 'where the hole reads as zeroes');

# A table with holes: a scan reads them as empty pages, and VACUUM gives
# them no block; rows inserted later fill them, each given its block.
sql(q{
	CREATE TABLE u (a int);
	INSERT INTO u SELECT generate_series(1, 1000);
});
my $u = sql(q{SELECT pg_relation_filenode('u')});
my $ublocks = sql(q{SELECT pg_relation_size('u') / 8192});
durable(qq{SELECT test_bmap_extend_at($u, 200)});
my $umapped = sql(qq{SELECT count(*) FROM test_bmap_mapped($u)});
is($umapped, $ublocks + 1, 'a table with a hole of many blocks');
is(sql(q{SELECT count(*), sum(a) FROM u}), '1000|500500',
	'a scan reads past the hole');
# (not truncated: the page at the end is empty)
sql(q{VACUUM (TRUNCATE false) u});
is(sql(qq{SELECT count(*) FROM test_bmap_mapped($u)}), $umapped,
	'VACUUM gives the hole no block');
sql(q{INSERT INTO u SELECT generate_series(1001, 30000)});
is(sql(q{SELECT count(*), sum(a) FROM u}), '30000|450015000',
	'rows inserted after');
is(sql(q{SELECT pg_relation_size('u') / 8192}), '201',
	'fill the hole, the relation growing no longer');
cmp_ok(sql(qq{SELECT count(*) FROM test_bmap_mapped($u)}), '>', $umapped,
	'its blocks given blocks as they are filled');
$node->stop('immediate');
$node->start;
is(sql(q{SELECT count(*), sum(a) FROM u}), '30000|450015000',
	'crash recovery keeps them');
$node->wait_for_catchup($standby);
is($standby->safe_psql('postgres', q{SELECT count(*), sum(a) FROM u}),
	'30000|450015000', 'as does the standby');

# The limits of extension, as md.c's: zero-extension (as the buffer
# manager extends) stops short of a length of InvalidBlockNumber, which the
# storage manager's cached length would read as unknown, while a write of
# the last block, MaxBlockNumber, reaches it; a write of InvalidBlockNumber
# fails.
sql(q{CREATE TABLE v (a int)});
my $v = sql(q{SELECT pg_relation_filenode('v')});
durable(qq{SELECT test_bmap_extend_at($v, @{[$last - 1]})});
is(sql(q{SELECT pg_relation_size('v') / 8192}), $last,
	'a relation one block short of the longest');
my ($ret, $out, $err) = $node->psql('postgres',
	qq{SELECT test_bmap_extend($v, 1)});
isnt($ret, 0, 'zero-extension to a length of InvalidBlockNumber fails');
like($err, qr/cannot extend file .* beyond 4294967295 blocks/,
	'as md.c refuses it');
is(sql(q{SELECT pg_relation_size('v') / 8192}), $last,
	'leaving the length as it was');
($ret, $out, $err) = $node->psql('postgres',
	qq{SELECT test_bmap_extend_at($v, @{[$last + 1]})});
isnt($ret, 0, 'a write of block InvalidBlockNumber fails');
like($err, qr/cannot extend file .* beyond 4294967295 blocks/,
	'before anything is mapped for it');
durable(qq{SELECT test_bmap_extend_at($v, $last)});
is(sql(q{SELECT pg_relation_size('v') / 8192}), $last + 1,
	'a write of the last block reaches the longest length');
is(mapped(undef, $v), ($last - 1) . ",$last", 'mapping that block alone');
$node->wait_for_catchup($standby);
is($standby->safe_psql('postgres', q{SELECT pg_relation_size('v') / 8192}),
	$last + 1, 'as the standby replays');
is(mapped($standby, $v), ($last - 1) . ",$last", 'with the map');

$standby->stop;
$node->stop;
done_testing();
