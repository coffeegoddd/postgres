# Copyright (c) 2026, PostgreSQL Global Development Group

# A repair and a full collection take the history horizon's lock before
# the chunk store's (which a repair's receive holds to its transaction's
# end, and a collection takes as it begins), so the two wait for one
# another, never deadlock.  A repair stopped after it received, with a
# full collection begun meanwhile: the collection waits for the horizon,
# the repair goes on to commit, then the collection runs, keeping what was
# repaired.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('rc');
$node->init;
$node->append_conf('postgresql.conf', 'deadlock_timeout = 100ms');
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE rc');
my $url = 'file://' . PostgreSQL::Test::Utils::tempdir() . '/remote';

sub sql { return $node->safe_psql('rc', $_[0]); }

sql(q{CREATE EXTENSION injection_points});
sql(q{CREATE SCHEMA rc VERSIONED;
	CREATE TABLE rc.t (id int PRIMARY KEY, v text);
	INSERT INTO rc.t SELECT g, 'v' FROM generate_series(1, 200) g;});
for my $d (1 .. 6)
{
	sql(qq{UPDATE rc.t SET v = 'd$d' WHERE id % 7 = $d;
		SELECT dolt_commit('-Am', 'd$d', '--date', '2020-01-0$d')});
}
sql(qq{SELECT dolt_remote('add', 'origin', '$url');
	SELECT dolt_push('origin', 'main')});
sql(q{SELECT commits FROM dolt_history_drop('2020-01-04')});
sql(q{SELECT * FROM prolly_gc(true)});
my $top = sql(q{SELECT tops[1] FROM prolly_history_horizon()});
my $read = qq{SELECT count(*) FROM dolt_at('$top', NULL::rc.t)};
my ($r0, $o0, $e0) = $node->psql('rc', $read);
like($e0, qr/is past the history horizon/, 'the severed data is freed');

# The repair stops once it has received.
my $repair = $node->background_psql('rc', on_error_stop => 0);
$repair->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-repair-received', 'wait')});
$repair->query_until(qr/started/, q{\echo started
SELECT dolt_history_repair('origin') AS repaired;
\echo repaired
});
$node->poll_query_until('rc', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-repair-received'})
  or die 'the repair did not stop';

# A full collection begun now waits for the horizon.
my $gc = $node->background_psql('rc', on_error_stop => 0);
$gc->query_until(qr/started/, q{\echo started
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
\echo collected
});
$node->poll_query_until('rc', q{SELECT count(*) > 0 FROM pg_locks
	WHERE NOT granted AND locktype = 'object'})
  or die 'the collection did not wait for the horizon';

sql(q{SELECT injection_points_wakeup('prolly-repair-received')});
like($repair->query_until(qr/repaired/, ''), qr/1/, 'the repair went on');
$repair->quit;
like($gc->query_until(qr/collected/, ''), qr/t/, 'the collection ran after');
$gc->quit;

cmp_ok(sql($read), '>', 0, 'the history repaired is kept');
ok(!$node->log_contains(qr/deadlock detected/), 'no deadlock');

done_testing();
