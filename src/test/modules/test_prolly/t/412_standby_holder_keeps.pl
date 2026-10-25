# Copyright (c) 2026, PostgreSQL Global Development Group

# A hot standby's transaction already holding pointers read from history
# goes on reading long values while the replay of a swap waits for it: it
# doesn't wait for the replay (which waits for it), so neither stalls for
# good, even with max_standby_streaming_delay = -1.  Replay goes on once
# it ends.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('holder_keeps');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	CREATE SCHEMA hk VERSIONED;
	CREATE TABLE hk.k (id int PRIMARY KEY, j jsonb);
	SELECT dolt_commit('-Am', 'empty')});

# a branch whose history alone names its long values
sub side_branch
{
	my ($name) = @_;
	sql(qq{SELECT dolt_branch('$name');
		SELECT dolt_checkout('$name');
		INSERT INTO hk.k SELECT g, jsonb_build_object('a',
				(SELECT string_agg(md5(g || '$name' || i), '') FROM generate_series(1, 200) i),
				'g', g)
			FROM generate_series(1, 5) g;
		SELECT dolt_commit('-am', 'values');
		DELETE FROM hk.k;
		SELECT dolt_commit('-am', 'gone');
		SELECT dolt_checkout('main')});
}
my $read = q{SELECT id, md5(j::text) FROM
	(SELECT id, j FROM dolt_at('%s~1', NULL::hk.k) ORDER BY id) s};
side_branch('side');
side_branch('other');
my $other = sql(sprintf(q{SELECT count(*), string_agg(md5, ',' ORDER BY id) FROM (%s) s},
	sprintf($read, 'other')));

$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('holder_keeps_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
hot_standby_feedback = on
max_standby_streaming_delay = -1
});
$standby->start;
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

# a holder: its transaction read pointers of 'side'
my $c = $standby->background_psql('postgres', on_error_stop => 0);
$c->query_safe(sprintf(q{BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE c CURSOR FOR %s; FETCH 1 FROM c}, sprintf($read, 'side')));

# 'side' dropped and collected, the collection held after its swap
sql(q{SELECT dolt_branch('-D', 'side')});
sql('VACUUM');
my $g = $node->background_psql('postgres');
$g->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-retire', 'wait')});
$g->query_until(qr/started/, q{
	\echo started
	SELECT dolt_gc('--full') IS NOT NULL;
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-gc-retire'}) or die 'the collection did not stop at its retire';

# the swap's replay waits for the holder (with -1, until it ends)
$standby->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'startup' AND wait_event = 'RecoveryConflictChunkStore'})
  or die 'the replay did not wait for the holder';

# the holder reads another branch's long values: keep()s during the replay
$c->query_until(qr/sent/, sprintf(q{\echo sent
	SELECT count(*), string_agg(md5, ',' ORDER BY id) FROM (%s) s;
	\echo read
}, sprintf($read, 'other')));
my $active = q{SELECT count(*) FROM pg_stat_activity
	WHERE backend_type = 'client backend' AND state = 'active'
	AND pid <> pg_backend_pid()};
for (1 .. 100)
{
	last if $standby->safe_psql('postgres', $active) eq '0';
	usleep(100_000);
}
is($standby->safe_psql('postgres', $active), '0',
	'the holder\'s read isn\'t left waiting on the replay that waits on it');
# (with it left waiting, cancelled, so the test ends)
$standby->safe_psql('postgres', q{SELECT pg_cancel_backend(pid) FROM pg_stat_activity
	WHERE wait_event = 'ProllySwapReplay'});
my $got = $c->query_until(qr/read/, '');
$got =~ s/\n?read\n?$//;
is($got, $other, 'and reads every value');
is($standby->safe_psql('postgres', q{SELECT wait_event FROM pg_stat_activity
	WHERE backend_type = 'startup'}), 'RecoveryConflictChunkStore',
	'while the replay still waits for it');

# the holder ends: replay goes on
$c->query_safe('COMMIT');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
pass('replay caught up once the holder ended');

sql(q{SELECT injection_points_wakeup('prolly-gc-retire');
	SELECT injection_points_detach('prolly-gc-retire')});
$g->query_until(qr/done/, '');
$g->quit;
$c->quit;
$standby->stop;
$node->stop;
done_testing();
