# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum's automatic fold (prolly.autofold_threshold) never waits for
# a branch's refs: it folds before taking them, as the fold worker does,
# and gives up, to try again next time, while another holds them; so a
# commit never waits for it either.  And the fold worker's launcher takes
# the databases written in turn, a worker leaving after a number of
# passes: a database written without pause doesn't keep the only slot
# from another for good.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('autofold_defers');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
prolly.autofold_threshold = 0
prolly_fold_worker = off
log_min_messages = debug1
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v int);
	INSERT INTO s.t SELECT g, 0 FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'base');
});
my $log = sql(q{SELECT 'pg_toast.pg_prolly_log_' || 's.t'::regclass::oid});

# The log past the threshold, and the branch's refs held by a session's
# open transaction, begun after the writes (they are what it sees), before
# autovacuum's folds are turned on: autovacuum neither waits for the refs
# nor folds.
sql(q{UPDATE s.t SET v = 1 WHERE id <= 3000});
my $holder = $node->background_psql('postgres');
$holder->query_safe(q{BEGIN});
$holder->query_safe(q{SELECT dolt_reset('--soft', 'HEAD')});
my $offset = -s $node->logfile;
$node->append_conf('postgresql.conf', 'prolly.autofold_threshold = 0.05');
$node->reload;
my $waited = 0;
for (1 .. 80)
{
	$waited = 1
	  if sql(q{SELECT count(*) FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker' AND wait_event_type = 'Lock'}) > 0;
	usleep(100_000);
}
is($waited, 0, 'autovacuum waits for no lock while the refs are held');
like(slurp_file($node->logfile, $offset), qr/automatic fold of database "postgres" given up/,
	'it gives its fold up');
is(sql(qq{SELECT count(*) > 0 FROM $log}), 't', 'the log keeps the writes');
$holder->query_safe(q{COMMIT});
$holder->quit;
ok($node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM $log}),
	'and folds once they are free');

# The fold worker, one slot: a database written without pause and another
# written once.  The other's writes are folded all the same.
$node->append_conf('postgresql.conf', qq{
prolly.autofold_threshold = 0
prolly_fold_worker = on
prolly_fold_worker_max_workers = 1
prolly_fold_worker_naptime = 100ms
});
$node->reload;
sql(q{CREATE DATABASE other});
$node->safe_psql('other', q{
	CREATE SCHEMA o VERSIONED;
	CREATE TABLE o.t (id int PRIMARY KEY, v int);
	INSERT INTO o.t SELECT g, 0 FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'base');
});
sql(q{CREATE PROCEDURE public.busy(n int) LANGUAGE plpgsql AS $$
	BEGIN
		FOR i IN 1..n LOOP
			UPDATE s.t SET v = v + 1 WHERE id = 1 + i % 1000;
			COMMIT;
			PERFORM pg_sleep(0.02);
		END LOOP;
	END $$});
my $busy = $node->background_psql('postgres');
$busy->query_until(qr/started/, "\\echo started\nCALL busy(2000);\n");
usleep(1_000_000);
my $olog = $node->safe_psql('other',
	q{SELECT 'pg_toast.pg_prolly_log_' || 'o.t'::regclass::oid});
$node->safe_psql('other', q{UPDATE o.t SET v = 1});
ok( $node->poll_query_until('other', qq{SELECT count(*) = 0 FROM $olog}),
	'another database\'s writes are folded while one is written without pause');
is(sql(q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE query LIKE 'CALL busy%' AND state = 'active'}), 't',
	'that one still being written');
$busy->query_safe(q{SELECT 1});
$busy->quit;

done_testing();
