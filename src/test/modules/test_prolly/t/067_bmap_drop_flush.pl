# Copyright (c) 2026, PostgreSQL Global Development Group

# Dropping all of a database's or relation's buffers while a checkpoint
# writes one of them out.  Writing a block-mapped page looks it up on its
# map page, from shared buffers or else from disk, where a map page dirtied
# since the last checkpoint may be stale or, newly added, all zeroes.  So a
# drop leaves the map's buffers until the pages they map are gone, waiting
# for one being written out; and the writeback hint a checkpoint sends for
# a page it wrote, later, uses a map page only if it is cached.
#
# Each case needs the map pages in buffers before the pages they map, the
# order a drop going through the buffers in order would reach them in: a
# database copied from a template with block-mapped tables adds each
# relation's map pages before reading its pages into buffers, and a
# standby replays a relation's extension before the page it adds.  The
# test checks the order first.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('drop_flush');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
shared_buffers = 16MB
bgwriter_lru_maxpages = 0
fsync = on
checkpoint_flush_after = 8kB
});
$node->start;

$node->safe_psql('postgres', q{CREATE EXTENSION injection_points;
	CREATE EXTENSION pg_buffercache;
	CREATE EXTENSION test_prolly});
$node->safe_psql('postgres', 'CREATE DATABASE src');
$node->safe_psql('src', q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.t (a int8 PRIMARY KEY, b int8);
	INSERT INTO cow.t SELECT g, g FROM generate_series(1, 1000) g;
});
$node->safe_psql('postgres', 'CHECKPOINT');

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Does every block-mapped relation of 'where' (a condition on
# pg_buffercache) have its map pages in buffers before its pages?
sub maps_first
{
	my ($n, $where) = @_;
	return $n->safe_psql(
		'postgres', qq{
		SELECT count(*) > 0 AND bool_and(m.last < p.first)
		FROM (SELECT relfilenode, max(bufferid) AS last FROM pg_buffercache
			WHERE $where AND relforknumber = 4 GROUP BY 1) m
		JOIN (SELECT relfilenode, min(bufferid) AS first FROM pg_buffercache
			WHERE $where AND relforknumber = 0 GROUP BY 1) p
		USING (relfilenode)});
}

# A copy of the template, its pages and map pages dirty, its map pages all
# zeroes on disk.
sub copy
{
	my ($db) = @_;
	sql("CREATE DATABASE $db TEMPLATE src STRATEGY wal_log");
	# (its pages, copied through a small ring of buffers, may be written
	# out already: a change reads them in again, after the map pages)
	$node->safe_psql($db, 'UPDATE cow.t SET b = b');
	is( maps_first(
			$node, qq{reldatabase =
			(SELECT oid FROM pg_database WHERE datname = '$db')}),
		't',
		"$db: the copy's map pages are in buffers before its pages");
}

# Start a checkpoint (on 'n'), stopped writing out the first block-mapped
# page, or at 'point' after it.
sub stopped_checkpoint
{
	my ($n, $point) = @_;
	$point //= 'bmap-write';
	$n->safe_psql('postgres',
		qq{SELECT injection_points_attach('$point', 'wait')});
	my $c = $n->background_psql('postgres', on_error_stop => 0);
	$c->query_until(qr/started/, q{
		\echo started
		CHECKPOINT;
		\echo checkpointed
	});
	$n->poll_query_until('postgres', qq{SELECT count(*) > 0
		FROM pg_stat_activity
		WHERE backend_type = 'checkpointer' AND wait_event = '$point'})
	  or die 'the checkpoint did not stop';
	return $c;
}

sub wake
{
	my ($n, $point) = @_;
	$n->safe_psql('postgres', qq{SELECT injection_points_detach('$point');
		SELECT injection_points_wakeup('$point')});
}

# The checkpoint on 'n' went on and succeeded, finding every map page.
sub checkpointed
{
	my ($n, $c, $log_offset, $what) = @_;
	$c->query_until(qr/checkpointed/, '');
	unlike($c->{stderr}, qr/request failed/, "$what: the checkpoint succeeds");
	ok( !$n->log_contains('block map page|has no page for block',
			$log_offset),
		"$what: every page it wrote found its map page");
	$c->quit;
}

# A database dropped while a checkpoint writes out one of its pages: the
# drop waits for the write.
copy('dst');
my $c = stopped_checkpoint($node);
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
wake($node, 'bmap-write');
$d->query_until(qr/dropped/, '');
$d->quit;
checkpointed($node, $c, $log_offset, 'a database dropped');

# A database dropped after a checkpoint wrote out one of its pages, before
# it sent the page's writeback hint (at once, checkpoint_flush_after being
# a page) and before it wrote out the map pages.
copy('dst2');
$c = stopped_checkpoint($node, 'bmap-writeback');
$d = $node->background_psql('postgres', on_error_stop => 0);
$d->query_until(qr/started/, q{
	\echo started
	DROP DATABASE dst2;
	\echo dropped
});
# (it has dropped the buffers once it waits for a checkpoint of its own)
$node->poll_query_until('postgres', q{SELECT count(*) > 0
	FROM pg_stat_activity
	WHERE query LIKE 'DROP DATABASE dst2%'
	AND lower(wait_event) LIKE 'checkpoint%'})
  or die 'the drop did not drop the buffers';
$log_offset = -s $node->logfile;
wake($node, 'bmap-writeback');
$d->query_until(qr/dropped/, '');
$d->quit;
checkpointed($node, $c, $log_offset, 'a writeback after the drop');

is( sql(q{SELECT count(*) FROM pg_database WHERE datname LIKE 'dst%'}),
	'0', 'the databases are dropped');
is($node->safe_psql('src', 'SELECT count(*), sum(b) FROM cow.t'),
	'1000|500500', 'the template reads as before');

# A relation dropped on a standby while a restartpoint writes out one of
# its pages.  Recovery drops a large relation's buffers by going through
# them all.
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('drop_flush_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
shared_buffers = 16MB
bgwriter_lru_maxpages = 0
});
$standby->start;

sql(q{CREATE SCHEMA d VERSIONED;
	CREATE TABLE d.big (a int8, b int8);
	INSERT INTO d.big SELECT g, g FROM generate_series(1, 30000) g;
	CHECKPOINT});
$node->wait_for_catchup($standby, 'replay', $node->lsn('flush'));
my $rel = sql(q{SELECT pg_relation_filenode('d.big')});
cmp_ok(sql(q{SELECT pg_relation_size('d.big') / 8192}), '>', 2048 / 32,
	'the relation is larger than a drop looks up block by block');
is(maps_first($standby, "relfilenode = $rel"),
	't', "the standby has the relation's map pages in buffers before its pages");

$c = stopped_checkpoint($standby);
sql('DROP TABLE d.big');
$standby->poll_query_until('postgres', q{SELECT count(*) > 0
	FROM pg_stat_activity
	WHERE backend_type = 'startup' AND lower(wait_event) = 'bufferio'})
  or die 'the replay did not wait for the write';
$log_offset = -s $standby->logfile;
wake($standby, 'bmap-write');
checkpointed($standby, $c, $log_offset, 'a relation dropped on a standby');
$node->wait_for_catchup($standby, 'replay', $node->lsn('flush'));
is( $standby->safe_psql('postgres',
		qq{SELECT count(*) FROM pg_buffercache WHERE relfilenode = $rel}),
	'0', 'the standby has none of its buffers');

$standby->stop;
$node->stop;
done_testing();
