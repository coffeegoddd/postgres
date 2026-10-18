# Copyright (c) 2026, PostgreSQL Global Development Group

# Dropping a database's buffers while a checkpoint writes one of them out.
# Writing a block-mapped page looks it up on its map page, from shared
# buffers or else from disk, where a map page dirtied since the last
# checkpoint may be stale or, newly added, all zeroes.  So the drop leaves
# the map's buffers until the pages they map are gone, waiting for one
# being written out.
#
# A database copied from a template with block-mapped tables gives the
# order that exposes this: each copied relation's map pages are added
# before its pages are read into buffers, so a drop going through the
# buffers in order would reach a map page before a page it maps.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('drop_database_flush');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
bgwriter_lru_maxpages = 0
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE DATABASE src');
$node->safe_psql('src', q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.t (a int8 PRIMARY KEY, b int8);
	INSERT INTO cow.t SELECT g, g FROM generate_series(1, 1000) g;
});
$node->safe_psql('postgres', 'CHECKPOINT');

# The copy's pages and map pages are dirty, its map pages all zeroes on disk.
$node->safe_psql('postgres',
	'CREATE DATABASE dst TEMPLATE src STRATEGY wal_log');
is($node->safe_psql('dst', 'SELECT count(*), sum(b) FROM cow.t'),
	'1000|500500', 'the copy reads as its template');

# A checkpoint stops writing out the copy's first block-mapped page.
$node->safe_psql('postgres',
	q{SELECT injection_points_attach('bmap-write', 'wait')});
my $c = $node->background_psql('postgres', on_error_stop => 0);
$c->query_until(qr/started/, q{
	\echo started
	CHECKPOINT;
	\echo checkpointed
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0
	FROM pg_stat_activity
	WHERE backend_type = 'checkpointer' AND wait_event = 'bmap-write'})
  or die 'the checkpoint did not stop';

# The drop waits for that write.
my $d = $node->background_psql('postgres', on_error_stop => 0);
$d->query_until(qr/started/, q{
	\echo started
	DROP DATABASE dst;
	\echo dropped
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0
	FROM pg_stat_activity
	WHERE query LIKE 'DROP DATABASE dst%' AND lower(wait_event) = 'bufferio'})
  or die 'the drop did not wait for the write';

my $log_offset = -s $node->logfile;
$node->safe_psql('postgres',
	q{SELECT injection_points_detach('bmap-write');
	SELECT injection_points_wakeup('bmap-write')});

my $out = $c->query_until(qr/checkpointed/, '');
$d->query_until(qr/dropped/, '');
unlike($c->{stderr}, qr/checkpoint request failed/,
	'the checkpoint succeeds');
ok( !$node->log_contains('block map page|has no page for block',
		$log_offset),
	'every page it wrote found its map page');
$c->quit;
$d->quit;

is( $node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_database WHERE datname = 'dst'}),
	'0', 'the database is dropped');
$node->safe_psql('postgres', 'CHECKPOINT');
is($node->safe_psql('src', 'SELECT count(*), sum(b) FROM cow.t'),
	'1000|500500', 'the template reads as before');

done_testing();
