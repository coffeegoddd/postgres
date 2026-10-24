# Copyright (c) 2026, PostgreSQL Global Development Group

# The fold worker batches a busy branch's writes: while they go on, a
# branch is folded only once prolly_fold_worker_batch_rows rows were
# written since its last fold, or once the first of them waited
# prolly_fold_worker_max_delay; once they stop, at the next pass.  A batch
# of 0 rows folds at every pass, as the worker did before.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep time);

my $node = PostgreSQL::Test::Cluster->new('fold_worker_batching');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = on
prolly_fold_worker_naptime = 1s
prolly_fold_worker_batch_rows = 500
prolly_fold_worker_max_delay = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub folds
{
	return sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'fold_worker_folds'});
}

sub deferred
{
	return sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'fold_worker_deferred'});
}

sub set_conf
{
	my ($name, $value) = @_;
	sql("ALTER SYSTEM SET $name = '$value'");
	sql(q{SELECT pg_reload_conf()});
	usleep(200_000);
}

sql(q{
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v int);
	INSERT INTO s.t SELECT g, 0 FROM generate_series(1, 1000) g;
	SELECT dolt_commit('-Am', 'base');
	CREATE PROCEDURE public.trickle(n int, per int) LANGUAGE plpgsql AS $$
	BEGIN
		FOR i IN 1..n LOOP
			UPDATE s.t SET v = v + 1
			 WHERE id BETWEEN (i * per) % 1000 + 1 AND (i * per) % 1000 + per;
			COMMIT;
			PERFORM pg_sleep(0.01);
		END LOOP;
	END $$;
});
my $log = sql(q{SELECT 'pg_toast.pg_prolly_log_' || 's.t'::regclass::oid});
my $empty = qq{SELECT count(*) = 0 FROM $log};

# The setup's writes seen by the worker first, which then exits, so every
# fold counted below is the trickles'.
ok($node->poll_query_until('postgres', $empty), 'the base folded');
ok( $node->poll_query_until(
		'postgres', q{SELECT value > 0 FROM pg_stat_prolly
		  WHERE name = 'fold_worker_folds'}),
	'the worker saw the setup');
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
		  WHERE backend_type = 'prolly fold worker'}),
	'and exited');

my $writer = $node->background_psql('postgres');
my $writing = q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE query LIKE 'CALL trickle%' AND state = 'active'};

# Fewer rows than a batch, written without a pause the length of a pass:
# not folded while the writes go on.
my $before = folds();
my $deferred_before = deferred();
$writer->query_safe(q{CALL trickle(400, 1)});
is(folds() - $before, 0, 'a trickle of fewer rows than a batch is not folded');
cmp_ok(deferred() - $deferred_before, '>=', 2,
	'the worker left it for a later pass, pass after pass, each counted');
is(sql(qq{SELECT count(*) > 0 FROM $log}), 't', 'its log keeps the writes');

# Once the writes stop, at the next pass.
ok($node->poll_query_until('postgres', $empty), 'folded once they stop');
cmp_ok(folds() - $before, '>=', 1, 'by the worker');

# More rows than a batch: folded while the writes still go on.
$before = folds();
$writer->query_until(qr/started/, "\\echo started\nCALL trickle(400, 10);\n");
ok( $node->poll_query_until(
		'postgres', qq{SELECT value > $before FROM pg_stat_prolly
		  WHERE name = 'fold_worker_folds'}),
	'a batch of rows is folded');
is(sql($writing), 't', 'while the writes go on');
$writer->query_safe(q{SELECT 1});
ok($node->poll_query_until('postgres', $empty), 'and the rest once they stop');

# Rows never reaching a batch: folded once the first waited max_delay, not
# before.
set_conf('prolly_fold_worker_batch_rows', '1000000');
set_conf('prolly_fold_worker_max_delay', '3s');
$before = folds();
my $start = time;
$writer->query_until(qr/started/, "\\echo started\nCALL trickle(1000, 1);\n");
ok( $node->poll_query_until(
		'postgres', qq{SELECT value > $before FROM pg_stat_prolly
		  WHERE name = 'fold_worker_folds'}),
	'writes that waited max_delay are folded');
my $waited = time - $start;
is(sql($writing), 't', 'while the writes go on');
cmp_ok($waited, '>=', 3, 'not before max_delay');
$writer->query_safe(q{SELECT 1});
ok($node->poll_query_until('postgres', $empty), 'and the rest once they stop');

# A batch of 0 rows: every pass folds what was written.
set_conf('prolly_fold_worker_batch_rows', '0');
set_conf('prolly_fold_worker_max_delay', '1h');
$before = folds();
$writer->query_until(qr/started/, "\\echo started\nCALL trickle(1000, 1);\n");
ok( $node->poll_query_until(
		'postgres', qq{SELECT value > $before FROM pg_stat_prolly
		  WHERE name = 'fold_worker_folds'}),
	'a batch of 0 rows folds at every pass');
is(sql($writing), 't', 'while the writes go on');
$writer->query_safe(q{SELECT 1});
$writer->quit;

# What the worker folded is the working set: the commit records every row.
ok($node->poll_query_until('postgres', $empty), 'all folded');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)}),
	'1000', 'the working set holds every write');
sql(q{SELECT dolt_commit('-am', 'trickles')});
is(sql(q{SELECT sum(v) FROM s.t}), 400 + 400 * 10 + 1000 + 1000,
	'every write was kept');
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'clean after the commit');

# A branch whose only writer is still open (its entry new since a restart)
# has nothing to batch: the worker folds it as it is and exits, not
# waiting out the writer's transaction in one of its slots.
set_conf('prolly_fold_worker_batch_rows', '500');
$node->restart;
my $open = $node->background_psql('postgres');
$open->query_until(qr/started/, "\\echo started\nBEGIN; INSERT INTO s.t VALUES (5001, 0); SELECT pg_sleep(15); COMMIT;\n");
my $exited = 0;
my $seen = 0;
foreach my $i (1 .. 100)
{
	my $n = sql(q{SELECT count(*) FROM pg_stat_activity
		WHERE backend_type = 'prolly fold worker'});
	$seen = 1 if $n > 0;
	if ($seen && $n == 0) { $exited = 1; last; }
	usleep(100_000);
}
ok($seen, 'a worker started for the open writer\'s branch');
ok($exited, 'and exits while the writer stays open');
$open->quit;

done_testing();
