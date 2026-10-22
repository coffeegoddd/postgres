# Copyright (c) 2026, PostgreSQL Global Development Group

# prolly_fold_worker: branches' working sets folded in the background soon
# after they're written, each branch's own, so a commit after folds
# nothing; what the working set reads as is unchanged.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('fold_worker');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = on
prolly_fold_worker_naptime = 50ms
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t SELECT g, 'x' FROM generate_series(1, 1000) g;
	SELECT dolt_commit('-Am', 'base');
});
my $log = sql(q{SELECT 'pg_toast.pg_prolly_log_' || 's.t'::regclass::oid});
my $live = qq{SELECT count(*) = 0 FROM $log};

# Rows written: the worker folds them; the log holds none after.
sql(q{UPDATE s.t SET v = 'y' WHERE id <= 100});
ok($node->poll_query_until('postgres', $live), 'the worker folded the writes');
is(sql(q{SELECT count(*) FROM dolt_status}), '1', 'the table reads as changed');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)}),
	'100', 'its rows too');

# A commit after folds no row: they're the baseline's.
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT pg_stat_force_next_flush()});
my $before = $s->query_safe(
	q{SELECT value FROM pg_stat_prolly WHERE name = 'fold_rows_read'});
$s->query_safe(q{SELECT dolt_commit('-am', 'folded')});
$s->query_safe(q{SELECT pg_stat_force_next_flush()});
my $after = $s->query_safe(
	q{SELECT value FROM pg_stat_prolly WHERE name = 'fold_rows_read'});
is($after - $before, 0, 'the commit read no row');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::s.t)}),
	'100', 'and committed them');
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'clean after');

# Another branch's writes, folded on that branch.
sql(q{SELECT dolt_branch('other')});
$s->query_safe(q{SELECT dolt_checkout('other')});
$s->query_safe(q{UPDATE s.t SET v = 'z' WHERE id > 900});
my $folded = 0;
foreach my $i (1 .. 300)
{
	if ($s->query_safe($live) eq 't') { $folded = 1; last; }
	usleep(100_000);
}
ok($folded, "the other branch's writes folded");
is($s->query_safe(q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)}),
	'100', "the other branch reads its own rows");
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'main untouched');
$s->quit;

# The refs held by a session's open transaction: the worker never waits
# for them (its fold deleted log entries that holder may be waiting to
# delete: a wait would deadlock), and folds once they're free.
$s = $node->background_psql('postgres');
$s->query_safe(q{BEGIN});
$s->query_safe(q{SELECT dolt_reset('--soft', 'HEAD')});
sql(q{UPDATE s.t SET v = 'held' WHERE id <= 20});
usleep(1_000_000);
is( sql(q{SELECT count(*) FROM pg_stat_activity
		  WHERE backend_type = 'prolly fold worker' AND wait_event_type = 'Lock'}),
	'0', 'the worker waits for no lock while the refs are held');
$s->query_safe(q{COMMIT});
$s->quit;
ok($node->poll_query_until('postgres', $live),
	'and folds once they are free');
sql(q{SELECT dolt_commit('-am', 'held')});

# Off: nothing folds in the background.
$node->append_conf('postgresql.conf', 'prolly_fold_worker = off');
$node->reload;
sql(q{UPDATE s.t SET v = 'w' WHERE id = 1});
usleep(500_000);
is(sql(qq{SELECT count(*) > 0 FROM $log}), 't', 'off: the log keeps the entry');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)}),
	'1', 'which the next fold reads');

done_testing();
