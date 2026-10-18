# Copyright (c) 2026, PostgreSQL Global Development Group

# Block-mapped changes with few shared buffers, where any buffer read may
# evict a dirty page of the relation being changed.  Writing that page
# looks it up on its map page, share-locking it if cached, so a change
# reads every buffer it needs (the changes list's, the reference counts')
# before it locks the map page.  Whether a read here evicts such a page
# depends on timing; an assert-enabled build fails at any read made while
# a map page is locked for a change, so each path below (a page's first
# change after a fold, an extension over entries a truncation left) checks
# the order on every run.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('evict_lock');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
shared_buffers = 128kB
max_connections = 5
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA f VERSIONED;
	CREATE TABLE f.t (a int8 PRIMARY KEY, b int8);
	INSERT INTO f.t SELECT g, g FROM generate_series(1, 3000) g});
my $rel = sql(q{SELECT pg_relation_filenode('f.t')});
sql('CHECKPOINT');
my $pages = sql(q{SELECT pg_relation_size('f.t') / 8192});
my ($lineage, $epoch) = split /\|/,
  sql(qq{SELECT lineage, epoch FROM test_bmap_fold_begin($rel)});

# Each page's first change after the fold copies it and lists its old block,
# reading the changes list's pages while the pages before it are dirty.
sql(q{UPDATE f.t SET b = -b});
is(sql('SELECT count(*), sum(b) FROM f.t'), '3000|-4501500',
	'every page copied and listed');
is( sql(
		qq{SELECT count(*) FROM test_bmap_fold_changes($rel, $lineage, $epoch)
		WHERE kind = 'copy'}),
	$pages, 'each page once');

# Entries a truncation left past the end, born before a fold: the
# extension that reuses them lists them, reading the changes list's pages
# while the relation's pages it just filled are dirty.
sql(q{CREATE TABLE f.u (a int8 PRIMARY KEY, b int8);
	INSERT INTO f.u SELECT g, g FROM generate_series(1, 30000) g;
	DELETE FROM f.u WHERE ctid >= '(5,0)'});
sql('VACUUM f.u');
is(sql(q{SELECT pg_relation_size('f.u') / 8192}), '5', 'truncated');
my $kept = sql(q{SELECT count(*), sum(b) FROM f.u});
my $urel = sql(q{SELECT pg_relation_filenode('f.u')});
my ($ulineage, $uepoch) = split /\|/,
  sql(qq{SELECT lineage, epoch FROM test_bmap_fold_begin($urel)});
sql(q{INSERT INTO f.u SELECT g, g FROM generate_series(30001, 60000) g});
is(sql('SELECT count(*), sum(b) FROM f.u WHERE a <= 30000'), $kept,
	'the extension reuses them');
is(sql('SELECT count(*), sum(b) FROM f.u WHERE a > 30000'), '30000|1350015000',
	'with the rows added');
cmp_ok(
	sql(
		qq{SELECT count(*) FROM test_bmap_fold_changes($urel, $ulineage, $uepoch)
		WHERE kind = 'copy' AND blkno >= 5}),
	'>', 0,
	'and lists them');

$node->stop;
done_testing();
