# Copyright (c) 2026, PostgreSQL Global Development Group

# Automatic writer branches must be made before analysis takes locks on
# the protected source. Queue all creators behind refs to reproduce #719.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('writer_locks');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
statement_timeout = '20s'
});
$node->start;
$node->safe_psql('postgres', q{
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v int NOT NULL);
INSERT INTO app.t SELECT n, 0 FROM generate_series(1,8) n;
SELECT dolt_commit('-Am', 'seed');
SELECT dolt_writer_mode('branch-auto');
});

for my $isolation ('READ COMMITTED', 'REPEATABLE READ')
{
	for my $count (2, 4, 8)
	{
		my $gate = $node->background_psql('postgres');
		$gate->query_safe("BEGIN; SELECT dolt_branch('gate$count')");
		my (@clients, @pids);
		for my $id (1 .. $count)
		{
			my $client = $node->background_psql('postgres');
			push @clients, $client;
			push @pids, $client->query_safe('SELECT pg_backend_pid()');
			$client->query_safe(qq{
	SET client_min_messages = warning;
	SET dolt.writer = 'worker$id';
	SET dolt.transaction_commit = on;
	BEGIN ISOLATION LEVEL $isolation;
	SELECT count(*) FROM app.t;
	});
			my $write = "UPDATE app.t SET v = v + 1 WHERE id = $id";
			$write = "WITH changed AS ($write RETURNING id) SELECT * FROM changed"
				if $count == 2;
			$write = "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) $write"
				if $count == 4;
			$client->query_until(qr/queued/, qq{
	\\echo queued
	$write;
	COMMIT;
	});
		}
		my $pids = join(',', @pids);
		ok($node->poll_query_until('postgres', qq{
	SELECT count(*) = $count FROM pg_stat_activity
	WHERE pid IN ($pids) AND wait_event_type = 'Lock' AND wait_event = 'object'
	}), "$count creators queue behind refs");
		is($node->safe_psql('postgres', qq{
	SELECT count(*) FROM pg_locks WHERE pid IN ($pids)
	AND locktype = 'relation' AND mode = 'RowExclusiveLock' AND granted
	AND relation = 'app.t'::regclass
	}), '0', 'queued creators do not hold write locks on the source');
		$gate->query_safe('ROLLBACK');
		$gate->quit;
		for my $id (1 .. $count)
		{
			my $client = $clients[$id - 1];
			like($client->query_safe('SELECT active_branch()'), qr/(?:^|\n)writer\/worker$id\/main$/, 
				"creator $id completes its write at concurrency $count");
		}
		for my $client (@clients)
		{
			$client->query_safe(q{
	SELECT dolt_integrate('-m', 'integrated');
	SELECT dolt_checkout('main');
	});
			$client->quit;
		}
		is($node->safe_psql('postgres',
			q{SELECT count(*) FROM dolt_branches() WHERE kind = 'writer'}),
			'0', 'integration and checkout clean up all writer branches');
	}
}
is($node->safe_psql('postgres', 'SELECT sum(v) FROM app.t'), '28',
	'all disjoint updates survive integration');
# Repeat the whole cycle with libpq prepared plans retained across branch
# creation, integration and cleanup. Revalidation must route before relocking.
{
	local $ENV{PGOPTIONS} = '-c dolt.transaction_commit=on -c client_min_messages=warning';
	$node->pgbench('-n -M prepared -c 4 -j 4 -t 3', 0,
		[qr/number of transactions actually processed: 12\/12/], [],
		'prepared statements complete every branch cycle',
		{ cycle => q{
BEGIN;
UPDATE app.t SET v = v + 1 WHERE id = :client_id + 1;
COMMIT;
SELECT dolt_integrate('-m', 'prepared cycle');
SELECT dolt_checkout('main');
} });
}
is($node->safe_psql('postgres', 'SELECT sum(v) FROM app.t'), '40',
	'all prepared-cycle updates are integrated');
is($node->safe_psql('postgres',
	q{SELECT count(*) FROM dolt_branches() WHERE kind = 'writer'}),
	'0', 'prepared cycles clean up their branches');
# A concurrent merge publishes source DDL while the creator waits for refs.
# READ COMMITTED must analyze against that newly committed schema; rolling
# back the first write must restore main and remove the tentative branch.
$node->safe_psql('postgres', q{
SELECT dolt_branch('ddl');
SELECT dolt_checkout('ddl');
ALTER TABLE app.t ADD COLUMN added int DEFAULT 7;
SELECT dolt_commit('-Am', 'new definition');
SELECT dolt_checkout('main');
});
my $ddl = $node->background_psql('postgres');
$ddl->query_safe(q{BEGIN; SELECT dolt_merge('ddl')});
my $writer = $node->background_psql('postgres');
my $pid = $writer->query_safe('SELECT pg_backend_pid()');
$writer->query_safe(q{SET dolt.writer = 'schema'; SET client_min_messages = warning; BEGIN;});
$writer->query_until(qr/schema_write/, q{
\echo schema_write
UPDATE app.t SET added = added + 1 WHERE id = 1;
});
ok($node->poll_query_until('postgres', qq{
SELECT wait_event = 'object' FROM pg_stat_activity WHERE pid = $pid
}), 'creator waits for source DDL publication before resolving its columns');
$ddl->query_safe('COMMIT');
$ddl->quit;
is($writer->query_safe('SELECT added FROM app.t WHERE id = 1'), '8',
	'analysis sees the column added by the concurrent source merge');
$writer->query_safe('ROLLBACK');
is($writer->query_safe('SELECT active_branch()'), 'main',
	'rollback restores the protected branch');
is($writer->query_safe('SELECT added FROM app.t WHERE id = 1'), '7',
	'rollback leaves the source value unchanged');
$writer->quit;
is($node->safe_psql('postgres',
	q{SELECT count(*) FROM dolt_branches() WHERE kind = 'writer'}),
	'0', 'rollback discards the automatic branch');
# Plain EXPLAIN does not run the write it explains, so it neither makes a
# writer branch nor moves the session off the protected branch.
for my $explain ('EXPLAIN (COSTS OFF)', 'EXPLAIN (ANALYZE false, COSTS OFF)')
{
	my $explainer = $node->background_psql('postgres');
	$explainer->query_safe(
		q{SET dolt.writer = 'explainer'; SET client_min_messages = warning});
	$explainer->query_safe("$explain UPDATE app.t SET v = v + 1 WHERE id = 1");
	is($explainer->query_safe('SELECT active_branch()'), 'main',
		"$explain of an update stays on the protected branch");
	$explainer->quit;
	is($node->safe_psql('postgres',
		q{SELECT count(*) FROM dolt_branches() WHERE kind = 'writer'}),
		'0', "$explain of an update makes no writer branch");
}
# A temporary CREATE TABLE AS whose query writes moves before analysis too:
# queued behind refs, it holds no write lock on the source.
{
	my $gate = $node->background_psql('postgres');
	$gate->query_safe("BEGIN; SELECT dolt_branch('gate_ctas')");
	my $ctas = $node->background_psql('postgres');
	my $ctas_pid = $ctas->query_safe('SELECT pg_backend_pid()');
	$ctas->query_safe(q{
SET dolt.writer = 'ctas';
SET client_min_messages = warning;
BEGIN;
SELECT count(*) FROM app.t;
});
	$ctas->query_until(qr/ctas_queued/, q{
\echo ctas_queued
CREATE TEMP TABLE changed AS
WITH m AS (UPDATE app.t SET v = v WHERE id = 3 RETURNING id) SELECT * FROM m;
});
	ok($node->poll_query_until('postgres', qq{
SELECT wait_event = 'object' FROM pg_stat_activity WHERE pid = $ctas_pid
}), 'temporary CREATE TABLE AS queues behind refs');
	is($node->safe_psql('postgres', qq{
SELECT count(*) FROM pg_locks WHERE pid = $ctas_pid
AND locktype = 'relation' AND mode = 'RowExclusiveLock' AND granted
AND relation = 'app.t'::regclass
}), '0', 'queued CREATE TABLE AS holds no write lock on the source');
	$gate->query_safe('ROLLBACK');
	$gate->quit;
	is($ctas->query_safe('SELECT active_branch()'), 'writer/ctas/main',
		'temporary CREATE TABLE AS writes on its writer branch');
	$ctas->query_safe('ROLLBACK');
	$ctas->quit;
}
# A sweep holds only the refs, not a snapshot that must see the branch's
# head: a repeatable read checkout still deletes a done writer branch after
# another session moved main since its snapshot.
my $done = $node->background_psql('postgres');
$done->query_safe(q{
SET dolt.writer = 'done';
SET dolt.transaction_commit = on;
SET client_min_messages = warning;
UPDATE app.t SET v = v + 1 WHERE id = 1;
SELECT dolt_integrate('-m', 'done');
});
$done->quit;
is($node->safe_psql('postgres',
	q{SELECT count(*) FROM dolt_branches() WHERE name = 'writer/done/main'}),
	'1', 'a done writer branch is left for a sweep');
my $rr = $node->background_psql('postgres', on_error_stop => 0);
$rr->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM app.t');
my $mover = $node->background_psql('postgres');
$mover->query_safe(q{
SET dolt.writer = 'mover';
SET dolt.transaction_commit = on;
SET client_min_messages = warning;
UPDATE app.t SET v = v + 1 WHERE id = 2;
SELECT dolt_integrate('-m', 'moved main');
});
my (undef, $failed) = $rr->query("SELECT dolt_checkout('main')");
is($failed, 0, 'repeatable read checkout sweeps after main moved unseen');
$rr->query('COMMIT');
$rr->quit;
is($node->safe_psql('postgres',
	q{SELECT count(*) FROM dolt_branches() WHERE name = 'writer/done/main'}),
	'0', 'the repeatable read sweep deletes the done writer branch');
# A sweep that deletes nothing (its only candidate is in use) lets the
# refs go: another session's branch creation does not wait for its COMMIT.
my $blocker = $node->background_psql('postgres');
$blocker->query_safe("BEGIN; SELECT dolt_checkout('main')");
my $ret = $node->psql('postgres',
	q{SET lock_timeout = '5s'; SELECT dolt_branch('free')});
is($ret, 0, 'a sweep that deletes nothing holds up no other refs mover');
$blocker->query_safe('COMMIT');
$blocker->quit;
$mover->query_safe("SELECT dolt_checkout('main')");
$mover->quit;
is($node->safe_psql('postgres',
	q{SELECT count(*) FROM dolt_branches() WHERE kind = 'writer'}),
	'0', 'leaving the in-use writer branch sweeps it');
$node->stop;
done_testing();
