# Copyright (c) 2026, PostgreSQL Global Development Group

# A standby replaying a commit that keeps its pages, then a change that
# copies the map path the kept pages share.  The standby writes out its
# root page naming the copied leaf, not the leaf, and crashes: its redo
# skips the kept pages' records from before the copy (tree_find()), and a
# standby writes no page at the snapshot, which it only replays.  What
# rebuilds those pages there is the images the snapshot logged of the
# pages changed since the last one (PRESERVE), written to their blocks in
# redo.  Promoted, the standby gives the kept pages back.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('kept_pages_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
wal_consistency_checking = all
shared_buffers = 64MB
wal_keep_size = 1GB
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
});
$primary->start;
$primary->safe_psql('postgres', q{CREATE EXTENSION pg_buffercache});
$primary->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('kept_pages_standby');
$standby->init_from_backup($primary, 'b', has_streaming => 1);
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.t};

# (pages of one leaf; the standby's restartpoint past them)
sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text) WITH (fillfactor = 10);
	INSERT INTO app.t SELECT g, 0, repeat('x', 100) FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'base');
});
sql('CHECKPOINT');
$primary->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');

# rows of the first pages changed and committed (kept), then a far page
sql(q{UPDATE app.t SET v = 1 WHERE id <= 40});
sql(q{SELECT dolt_commit('-am', 'first pages')});
my $at = sql($digest);
sql(q{UPDATE app.t SET v = -1 WHERE id = 2000});
my $now = sql($digest);
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $digest), $now, 'the standby replayed both');

# the standby's map naming the copied leaf written out, then a crash
is($standby->safe_psql('postgres', q{
	SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted) FROM pg_buffercache
	WHERE relfilenode = pg_relation_filenode('app.t')
		AND relforknumber = 4}),
	't', "the standby's map written out and evicted");
$standby->stop('immediate');
$standby->start;
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $digest), $now,
	'after a crash, the standby has the working set');

# promoted, it gives the kept pages back
$standby->promote;
$standby->safe_psql('postgres', q{SELECT dolt_reset('--hard')});
is($standby->safe_psql('postgres', $digest), $at,
	"the promoted standby's kept pages hold the commit's rows");

$standby->stop;
$primary->stop;
done_testing();
