# Copyright (c) 2026, PostgreSQL Global Development Group

# What reads a block-mapped relation's map in the background passes over a
# relation dropped as it reads, with no error.  A checkpoint's writeback
# hints, issued after the pages they name were written, find the relation
# dropped meanwhile and its map's file gone: they are skipped, and the
# checkpoint completes (the background writer issues its hints the same
# way).  And autovacuum's page sweep, having found a table's map, reads it
# after the table's drop has emptied and unlinked its files: the sweep
# passes over it, counting what it held from the release queue, and ends;
# another table reads whole after.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('dropped_map_readers');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
autovacuum_naptime = 1
prolly_fold_worker = off
bgwriter_lru_maxpages = 0
fsync = on  # (writeback hints are given only with fsync on)
checkpoint_flush_after = 8kB
checkpoint_timeout = 1h
max_wal_size = 1GB
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA kv VERSIONED;
	CREATE TABLE kv.keep (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.keep SELECT g, repeat('k', 100) FROM generate_series(1, 2000) g;
	CREATE TABLE kv.w (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.w SELECT g, repeat('w', 100) FROM generate_series(1, 2000) g;
	CHECKPOINT;
});

# w's pages dirtied, then written by a checkpoint, which stops before its
# first writeback hint; w is dropped meanwhile
sql(q{UPDATE kv.w SET v = repeat('u', 100)});
sql(q{SELECT injection_points_attach('bmap-writeback', 'wait')});
my $ckpt = $node->background_psql('postgres', on_error_stop => 0);
$ckpt->query_until(qr/started/, q{
	\echo started
	CHECKPOINT;
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'checkpointer' AND wait_event = 'bmap-writeback'})
  or die 'the checkpoint did not stop at its writeback';
my $logstart = -s $node->logfile;
sql(q{DROP TABLE kv.w});
sql(q{SELECT injection_points_detach('bmap-writeback');
	SELECT injection_points_wakeup('bmap-writeback')});
$ckpt->query_until(qr/done/, '');
unlike($ckpt->{stderr}, qr/ERROR/, 'the checkpoint completes');
$ckpt->quit;
my $log = slurp_file($node->logfile, $logstart);
unlike($log, qr/could not open file "[^"]*_bmap"/,
	'its hints for the dropped table open no map file');
is(sql(q{SELECT count(*) FROM kv.keep}), '2000', 'the other table reads');

# autovacuum's sweep (due once after a start) stopped at t's map, found;
# t dropped meanwhile, its files emptied and unlinked
$node->restart;
sql(q{
	CREATE TABLE kv.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.t SELECT g, repeat('t', 100) FROM generate_series(1, 3000) g;
	CHECKPOINT;
});
my $fn = sql(q{SELECT pg_relation_filenode('kv.t')});
sql(qq{SELECT injection_points_attach('bmap-sweep-relation', 'wait', '$fn')});
$logstart = -s $node->logfile;
sql(q{ALTER SYSTEM SET autovacuum = on; SELECT pg_reload_conf()});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker' AND wait_event = 'bmap-sweep-relation'})
  or die 'autovacuum\'s sweep did not stop at the table\'s map';
sql(q{DROP TABLE kv.t});
ok(!-e $node->data_dir . "/base/" . sql(q{SELECT oid FROM pg_database
	WHERE datname = 'postgres'}) . "/${fn}_bmap", 'the map\'s file is gone');
sql(q{SELECT injection_points_detach('bmap-sweep-relation');
	SELECT injection_points_wakeup('bmap-sweep-relation')});

# the sweep ends: the store swept, so no longer due
sql(q{ALTER SYSTEM SET autovacuum = off; SELECT pg_reload_conf()});
$node->poll_query_until('postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker'})
  or die 'autovacuum did not stop';
$log = slurp_file($node->logfile, $logstart);
unlike($log, qr/ERROR/, 'autovacuum\'s sweep reports no error');
is(sql(q{SELECT count(*) FROM kv.keep}), '2000', 'the other table reads');
is(sql(q{SELECT sum(length(v)) FROM kv.keep}), '200000', 'all of it');
is(sql(q{SELECT count(*) FROM prolly_page_sweep()}), '1', 'a later sweep runs');
is(sql(q{SELECT count(*) FROM kv.keep WHERE v = repeat('k', 100)}), '2000',
	'and the other table still reads whole');

$node->stop;
done_testing();
