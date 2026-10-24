# Copyright (c) 2026, PostgreSQL Global Development Group

# prolly_fold_worker: branches' working sets folded in the background soon
# after they're written, each branch's own, so a commit after folds
# nothing; what the working set reads as is unchanged.  The refs held, the
# worker neither waits nor folds; a fold's error is reported once, the
# branch passed over until written again; a dropped database is passed
# over, no worker started for it; at most prolly_fold_worker_max_workers
# run at once.

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
log_min_messages = debug1
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
my $read = q{SELECT pg_stat_force_next_flush();
	SELECT value FROM pg_stat_prolly WHERE name = 'fold_rows_read'};
my $retries = q{SELECT value FROM pg_stat_prolly WHERE name = 'fold_worker_retries'};
$s = $node->background_psql('postgres');
$s->query_safe(q{BEGIN});
$s->query_safe(q{SELECT dolt_reset('--soft', 'HEAD')});
my $reads_before = sql($read);
my $retries_before = sql($retries);
sql(q{UPDATE s.t SET v = 'held' WHERE id <= 20});
usleep(1_000_000);
my $tried = sql($retries) - $retries_before;
cmp_ok($tried, '>=', 2, 'a fold given up counts a retry');
# (backed off: tried after 1, 3, 7, 15... passes, not each of the ~20)
cmp_ok($tried, '<=', 8, 'tried again less and less often');
is( sql(q{SELECT count(*) FROM pg_stat_activity
		  WHERE backend_type = 'prolly fold worker' AND wait_event_type = 'Lock'}),
	'0', 'the worker waits for no lock while the refs are held');
is(sql($read) - $reads_before, 0, 'nor folds, to roll back, pass after pass');
$s->query_safe(q{COMMIT});
$s->quit;
ok($node->poll_query_until('postgres', $live),
	'and folds once they are free')
  or diag(sql(qq{SELECT xid, xmin, xmax FROM $log}));
sql(q{SELECT dolt_commit('-am', 'held')});

# A fold that errors: reported, and the branch passed over until it's
# written again (not met again every pass).
SKIP:
{
	skip 'injection points not supported by this build', 3
	  if $ENV{enable_injection_points} ne 'yes';
	sql(q{CREATE EXTENSION injection_points});
	sql(q{SELECT injection_points_attach('prolly-fold-worker-fold', 'error')});
	my $offset = -s $node->logfile;
	sql(q{UPDATE s.t SET v = 'erring' WHERE id = 30});
	usleep(1_000_000);
	my $errors = () = slurp_file($node->logfile, $offset) =~
	  /ERROR:  error triggered for injection point prolly-fold-worker-fold/g;
	is($errors, 1, 'a fold that errors is reported once, not every pass');
	sql(q{SELECT injection_points_detach('prolly-fold-worker-fold')});
	usleep(500_000);
	is(sql(qq{SELECT count(*) > 0 FROM $log}), 't', 'the branch passed over');
	sql(q{UPDATE s.t SET v = 'written again' WHERE id = 31});
	ok($node->poll_query_until('postgres', $live), 'until written again');
	sql(q{DROP EXTENSION injection_points});
}
sql(q{SELECT dolt_commit('-am', 'erred')});

# A dropped database is passed over: no worker started for it, to fail.
# (Written with the worker off, so its branch is still moved as it's
# dropped.)
$node->append_conf('postgresql.conf', 'prolly_fold_worker = off');
$node->reload;
sql(q{CREATE DATABASE gone});
$node->safe_psql('gone', q{CREATE SCHEMA g VERSIONED;
	CREATE TABLE g.t (id int PRIMARY KEY);
	INSERT INTO g.t VALUES (1)});
sql(q{DROP DATABASE gone});
my $offset = -s $node->logfile;
$node->append_conf('postgresql.conf', 'prolly_fold_worker = on');
$node->reload;
usleep(1_000_000);
unlike(slurp_file($node->logfile, $offset), qr/FATAL:  database \d+ does not exist/,
	'no worker for a dropped database');

# At most prolly_fold_worker_max_workers at once.
$node->append_conf('postgresql.conf', 'prolly_fold_worker_max_workers = 1');
$node->reload;
for my $db (qw(one two three))
{
	sql("CREATE DATABASE $db");
	$node->safe_psql($db, q{CREATE SCHEMA m VERSIONED;
		CREATE TABLE m.t (id int PRIMARY KEY, v text)});
}
my $most = 0;
for my $round (1 .. 3)
{
	$node->safe_psql($_, qq{INSERT INTO m.t SELECT g + $round * 10000, 'r'
		FROM generate_series(1, 2000) g}) for qw(one two three);
	for (1 .. 20)
	{
		my $n = sql(q{SELECT count(*) FROM pg_stat_activity
			WHERE backend_type = 'prolly fold worker'});
		$most = $n if $n > $most;
		usleep(20_000);
	}
}
cmp_ok($most, '<=', 1, 'never more fold workers than the setting');
for my $db (qw(one two three))
{
	my $mlog = $node->safe_psql($db,
		q{SELECT 'pg_toast.pg_prolly_log_' || 'm.t'::regclass::oid});
	ok($node->poll_query_until($db, qq{SELECT count(*) = 0 FROM $mlog}),
		"and ${db}'s writes folded, one database after another");
}

# A database the worker is folding in, copied as a template: the worker
# gives way (as autovacuum does), so the copy neither waits for it nor
# fails as the database is accessed by others.
SKIP:
{
	skip 'injection points not supported by this build', 2
	  if $ENV{enable_injection_points} ne 'yes';
	sql(q{CREATE DATABASE tmpl});
	$node->safe_psql('tmpl', q{CREATE EXTENSION injection_points;
		CREATE SCHEMA tp VERSIONED;
		CREATE TABLE tp.t (id int PRIMARY KEY, v text)});
	$node->safe_psql('tmpl',
		q{SELECT injection_points_attach('prolly-fold-worker-fold', 'wait')});
	$node->safe_psql('tmpl', q{INSERT INTO tp.t VALUES (1, 'x')});
	ok( $node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
			WHERE backend_type = 'prolly fold worker' AND datname = 'tmpl'
			AND wait_event = 'prolly-fold-worker-fold'}),
		'a worker folding in the template database');
	my $start = time;
	sql(q{CREATE DATABASE copied TEMPLATE tmpl});
	cmp_ok(time - $start, '<', 4, 'the copy doesn\'t wait for the worker');
	$node->safe_psql('tmpl',
		q{SELECT injection_points_detach('prolly-fold-worker-fold')});
}

# Off: nothing folds in the background.
$node->append_conf('postgresql.conf', 'prolly_fold_worker = off');
$node->reload;
sql(q{UPDATE s.t SET v = 'w' WHERE id = 1});
usleep(500_000);
is(sql(qq{SELECT count(*) > 0 FROM $log}), 't', 'off: the log keeps the entry');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)}),
	'1', 'which the next fold reads');

done_testing();
