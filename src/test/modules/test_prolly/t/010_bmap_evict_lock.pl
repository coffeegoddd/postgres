# Copyright (c) 2026, PostgreSQL Global Development Group

# Block-mapped changes with few shared buffers, where any buffer read may
# evict a dirty page of the relation being changed.  Writing that page
# looks it up on its map page, share-locking it if cached, so a change
# reads every buffer it needs (a new block's, the reference counts') before
# it locks the map page.  Whether a read here evicts such a page depends on
# timing; an assert-enabled build fails at any read made while a map page
# is locked for a change, so each path below (a shared page's first change,
# a truncation letting go of pages a copy shares) checks the order on every
# run.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('evict_lock');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
shared_buffers = 128kB
max_connections = 5
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int8 PRIMARY KEY, b int8);
	INSERT INTO t SELECT g, g FROM generate_series(1, 3000) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
sql('CHECKPOINT');
my $copy = sql(qq{SELECT test_bmap_share($rel)});
my $digest = sql(qq{SELECT test_bmap_rows_digest($copy)});

# Each page's first change after the share copies it to a new block,
# changing reference counts while the pages before it are dirty.
sql(q{UPDATE t SET b = -b});
is(sql('SELECT count(*), sum(b) FROM t'), '3000|-4501500',
	'every page copied');
is(sql(qq{SELECT count(*) FROM test_bmap_map($rel) WHERE refs <> 0}), '0',
	'none shared any more');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'and the copy is as it was');
sql(qq{SELECT test_bmap_unlink($copy)});

# Entries past a truncation's end that a copy shares: the truncation, once
# done, lets go of them, changing their counts before it locks the map
# page.  The pages are emptied and vacuumed before the share, so the
# truncating VACUUM after it needs to change none of them.
sql(q{CREATE TABLE u (a int8 PRIMARY KEY, b int8);
	INSERT INTO u SELECT g, g FROM generate_series(1, 30000) g;
	DELETE FROM u WHERE ctid >= '(5,0)'});
sql('VACUUM (TRUNCATE false) u');
my $kept = sql(q{SELECT count(*), sum(b) FROM u});
my $urel = sql(q{SELECT pg_relation_filenode('u')});
my $upages = sql(q{SELECT pg_relation_size('u') / 8192});
my $ucopy = sql(qq{SELECT test_bmap_share($urel)});
sql('VACUUM u');
is(sql(q{SELECT pg_relation_size('u') / 8192}), '5', 'truncated');
is( sql(
		qq{SELECT count(*) FROM test_bmap_map($ucopy)
		WHERE blkno >= 5 AND refs = 0}),
	$upages - 5,
	'the truncation let go of the copy\'s pages past the end');
sql(q{INSERT INTO u SELECT g, g FROM generate_series(30001, 60000) g});
is(sql('SELECT count(*), sum(b) FROM u WHERE a <= 30000'), $kept,
	'the table grows again');
is(sql('SELECT count(*), sum(b) FROM u WHERE a > 30000'), '30000|1350015000',
	'with the rows added');
sql(qq{SELECT test_bmap_unlink($ucopy)});

$node->stop;
done_testing();
