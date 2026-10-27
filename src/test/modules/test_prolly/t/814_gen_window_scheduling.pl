# Copyright (c) 2026, PostgreSQL Global Development Group

# The background's retires, scheduled by the history window.  The
# generation a retire is wanted for is reached past older ones not due,
# when an older one is due: the run goes on to it.  A retire held off
# while another backend severs is held off only so long
# (prolly.history_collect_interval), then runs all the same.  And a cancel
# of the autovacuum worker while it retires ends the visit, as a cancel
# does anywhere in it: it isn't taken for the retire's failure.

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

my $node = PostgreSQL::Test::Cluster->new('gen_window_scheduling');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
autovacuum_naptime = 1s
prolly_fold_worker = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE gw');

sub sql { return $node->safe_psql('gw', $_[0]); }

sql(q{CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points});
sql(q{CREATE SCHEMA gw VERSIONED;
	CREATE TABLE gw.t (id int PRIMARY KEY, v text);
	INSERT INTO gw.t SELECT g, md5(g::text) FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'c0')});
sql(q{SELECT * FROM test_chunk_gc(false)});
for my $i (1 .. 3)
{
	sql(qq{UPDATE gw.t SET v = md5(id::text || '$i') WHERE id % 4 = $i;
		SELECT dolt_commit('-Am', 'c$i')});
	sql(q{SELECT * FROM test_chunk_gc(false)});
}

# the three newest generations: the oldest of them due, the middle one not,
# the newest wanted
my @g = split /\n/, sql(q{SELECT epoch_lo FROM test_chunk_generations()
	WHERE settled ORDER BY epoch_lo DESC LIMIT 3});
my ($g3, $g2, $g1) = @g;
ok($g1 < $g2 && $g2 < $g3, 'three generations');
sql(qq{SELECT test_chunk_note_garbage(array(SELECT test_chunk_file_hashes(name)))
	FROM test_chunk_generations() WHERE settled AND epoch_lo = $g1});
is(sql(q{SELECT lo || ' ' || hi FROM test_chunk_retire_choose()}), "$g1 $g1",
	'unwanted, the run from the one due ends at the one not due');
my ($lo, $hi) = split / /,
  sql(qq{SELECT lo || ' ' || hi FROM test_chunk_retire_choose(NULL, NULL, $g3)});
ok($lo <= $g3 && $g3 <= $hi, 'wanted, it reaches the newest past the one not due');
is($lo, $g1, 'from the one due');
# (a budget of the wanted one's file: the run from the one due outgrows it)
my $b3 = sql(qq{SELECT f.bytes FROM test_chunk_table_files() f
	JOIN test_chunk_generations() g USING (name) WHERE g.settled AND g.epoch_lo = $g3});
($lo, $hi) = split / /, sql(
	qq{SELECT lo || ' ' || hi FROM test_chunk_retire_choose($b3, NULL, $g3)});
is($lo, $g3, 'with too small a budget for that run, the wanted one alone');
ok($g3 <= $hi, 'it is chosen');

# a retire wanted while another backend severs (the severs' turn taken,
# the refs written, as a sever's records write them): held off, the
# answer given at once, nothing written that would wait for the sever's
# transaction; then, once prolly.history_collect_interval has passed since
# the retire was wanted, not held off
# (delete mode: no archive's confirmation limits what a retire takes)
sql(q{ALTER DATABASE gw SET prolly.history_window_action = delete;
	ALTER DATABASE gw SET prolly.history_collect_interval = '1s'});
sql(qq{SELECT test_window_config_set('history_window.retire_wanted', $g3 + 1);
	SELECT test_window_config_set('history_window.retire_wanted_at',
		(extract(epoch FROM now() - '2000-01-01 00:00:00+00'::timestamptz) * 1000000)::int8)});
my $sever = $node->background_psql('gw');
$sever->query_safe(q{BEGIN; SELECT test_hold_sever_turn();
	SELECT test_window_config_set('history_window.test_sever_wrote', 1)});
my $due = q{SET statement_timeout = '5s';
	SELECT coalesce(max_hi::text, 'none') FROM test_window_retire_due()};
my ($ret, $out, $err) = $node->psql('gw', $due);
is($ret, 0, 'asked while another severs, the window answers at once') or diag($err);
is($out, '0', 'a retire is held off while another severs');
usleep(1_500_000);
($ret, $out, $err) = $node->psql('gw', $due);
is($ret, 0, 'and again') or diag($err);
is($out, 'none', 'not past prolly.history_collect_interval');
$sever->query_safe(q{COMMIT});
$sever->quit;
is(sql(q{SELECT coalesce(max_hi::text, 'none') FROM test_window_retire_due()}),
	'none', 'nor once the sever is done');

# the background retires: a cancel of the worker meanwhile
sql(q{CREATE TABLE visits (n int) USING heap WITH
	(autovacuum_vacuum_insert_threshold = 0,
	 autovacuum_vacuum_insert_scale_factor = 0)});
sql(q{ALTER DATABASE gw SET prolly.history_collect_interval = '1h'});
sql(q{SELECT injection_points_attach('prolly-gc-retire-begin', 'wait')});
my $offset = -s $node->logfile;
$node->safe_psql('postgres', q{ALTER SYSTEM SET autovacuum = on; SELECT pg_reload_conf()});
sql('INSERT INTO visits VALUES (1)');
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker' AND wait_event = 'prolly-gc-retire-begin'})
  or die 'the background retire did not stop';
my $pid = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker' AND wait_event = 'prolly-gc-retire-begin'});
$node->safe_psql('postgres', qq{SELECT pg_cancel_backend($pid)});
$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_stat_activity
	WHERE pid = $pid AND wait_event = 'prolly-gc-retire-begin'})
  or die 'the worker did not take the cancel';
$node->safe_psql('postgres', q{ALTER SYSTEM SET autovacuum = off; SELECT pg_reload_conf()});
$node->poll_query_until('postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker'})
  or die 'autovacuum did not stop';
sql(q{SELECT injection_points_detach('prolly-gc-retire-begin')});
my $log = slurp_file($node->logfile, $offset);

unlike($log, qr/automatic retire of generations \d+ to \d+ of database "gw" failed/,
	'the cancel isn\'t taken for the retire\'s failure');
like($log, qr/canceling/, 'it ends the visit');
is(sql(q{SELECT coalesce(max_hi::text, 'none') FROM test_window_retire_due()}),
	'none', 'and the retire isn\'t put off for a failure');

$node->stop;
done_testing();
