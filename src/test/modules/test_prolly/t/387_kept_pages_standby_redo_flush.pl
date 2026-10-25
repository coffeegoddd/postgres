# Copyright (c) 2026, PostgreSQL Global Development Group

# A streaming standby replaying commits that keep their pages.  Their own
# pages were logged as images, whose redo wrote them, so redo of each
# snapshot syncs nothing; it writes out the pages its walk of the
# snapshot's own leaves finds (one may have changed in place after its
# image), and visits no other buffer.  With few buffers, the standby writes
# kept pages out between their changes; crashed with its map written out
# and replayed again (where its map pages may be newer than the replay),
# then promoted, it gives the kept pages back.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('kept_redo_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
wal_consistency_checking = all
shared_buffers = 1MB
wal_keep_size = 1GB
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
prolly.restore_min_changes = 0
});
$primary->start;
$primary->safe_psql('postgres', q{CREATE EXTENSION pg_buffercache});
$primary->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('kept_redo_standby');
$standby->init_from_backup($primary, 'b', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'log_min_messages = debug1');
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.t};

sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text) WITH (fillfactor = 10);
	INSERT INTO app.t SELECT g, 0, repeat('x', 100) FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'base');
	CREATE TABLE public.churn (a int, pad text) WITH (fillfactor = 10);
	INSERT INTO public.churn SELECT g, repeat('c', 300) FROM generate_series(1, 3000) g;
});
sql('CHECKPOINT');
$primary->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');
my $log_offset = -s $standby->logfile;

# three commits, each of a page changed, other pages churned, and changed
# again; then the commit's kept page changed (moved off its block)
my @at;
for my $round (1 .. 3)
{
	my $id = $round * 7;
	sql(qq{UPDATE app.t SET v = $round WHERE id = $id});
	sql(q{UPDATE public.churn SET a = a + 1});
	sql(qq{UPDATE app.t SET v = $round WHERE id = $id + 1});
	sql(qq{SELECT dolt_commit('-am', 'round $round')});
	push @at, sql($digest);
	sql(qq{UPDATE app.t SET v = -$round WHERE id = $id + 2});
}
my $now = sql($digest);
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $digest), $now, 'the standby replayed them');
ok( !$standby->log_contains(
		'redo of a snapshot of .* writes out every buffer of it', $log_offset),
	"redo visited no buffer");
# (at this WAL level each kept snapshot's own pages were logged as images,
# whose redo wrote them: redo of the snapshot syncs nothing)
ok( $standby->log_contains(
		'redo of a snapshot of .* syncs nothing: its own pages were logged as images',
		$log_offset),
	"redo of each snapshot synced nothing, its own pages logged as images");

# the standby's map written out, then a crash
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

# promoted, it gives the last commit's kept pages back, and each earlier
# commit's rows on a reset to it
$standby->promote;
sub counted
{
	my ($name) = @_;
	return $standby->safe_psql('postgres', qq{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}
my $restored = counted('relations_restored');
$standby->safe_psql('postgres', q{SELECT dolt_reset('--hard')});
cmp_ok(counted('relations_restored'), '>', $restored,
	'the reset gave the kept pages back');
is($standby->safe_psql('postgres', $digest), $at[2], "and they hold the commit's rows");
for my $round (2, 1)
{
	my $hash = $standby->safe_psql('postgres',
		qq{SELECT commit_hash FROM dolt_log WHERE message = 'round $round'});
	$standby->safe_psql('postgres', qq{SELECT dolt_reset('--hard', '$hash')});
	is($standby->safe_psql('postgres', $digest), $at[$round - 1],
		"a reset to round $round has its commit's rows");
}

$standby->stop;
$primary->stop;
done_testing();
