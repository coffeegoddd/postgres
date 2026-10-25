# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum's automatic fold, too, trims the change-log rows it folds of
# a writer that committed while the fold was starting.  The fold looks
# through pg_class first, for a table whose log is due, by a snapshot of
# its own; held there by a table's lock while the writer runs, that
# snapshot is from before the writer committed.  The fold reads by one it
# takes as it begins, which sees the writer committed: had the scan's
# been kept under it, it would hold the trim back, the writer's rows would
# stay, and the fold after, finding nothing new to fold, would leave them
# in the log for good.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('autofold_trim_own_snapshot');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
autovacuum_naptime = 1s
prolly_fold_worker = off
prolly.autofold_threshold = 0.001
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# (a is ahead of t and b in pg_class: the scan reaches it first)
sql(q{
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.a (id int PRIMARY KEY, v int);
	CREATE TABLE s.t (id int PRIMARY KEY, v int);
	CREATE TABLE s.b (id int PRIMARY KEY, v int);
	INSERT INTO s.a VALUES (1, 0);
	INSERT INTO s.t SELECT g, 0 FROM generate_series(1, 100) g;
	INSERT INTO s.b SELECT g, 0 FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'base');
});
my $log = sql(q{SELECT 'pg_toast.pg_prolly_log_' || 's.t'::regclass::oid});
my $empty = qq{SELECT count(*) = 0 FROM $log};
is(sql($empty), 't', 'the log is empty to begin with');

# b written enough for its log to be due, committed; then a writer of a
# few rows of t, too few for its log ever to be due, running: the older
# transaction b's, so that it alone is below the horizon the running one
# holds.  a locked.
my $big = $node->background_psql('postgres');
$big->query_safe(q{BEGIN; UPDATE s.b SET v = 1});
my $writer = $node->background_psql('postgres');
$writer->query_safe(q{BEGIN; UPDATE s.t SET v = 1 WHERE id <= 10});
$big->query_safe('COMMIT');
$big->quit;
my $locker = $node->background_psql('postgres');
$locker->query_safe(q{BEGIN; LOCK TABLE s.a IN ACCESS EXCLUSIVE MODE});

# The automatic fold's scan waits for a's lock, its snapshot from while
# the writer runs.
sql(q{ALTER SYSTEM SET autovacuum = on; SELECT pg_reload_conf()});
ok( $node->poll_query_until(
		'postgres',
		q{SELECT count(*) > 0 FROM pg_locks l JOIN pg_stat_activity w USING (pid)
		  WHERE w.backend_type = 'autovacuum worker' AND NOT l.granted
		    AND l.relation = 's.a'::regclass}),
	"the automatic fold's scan waits for a");

# The writer commits, then a is let go: the fold goes on, b's log due,
# and folds the writer's rows too.
$writer->query_safe('COMMIT');
$writer->quit;
$locker->query_safe('COMMIT');
$locker->quit;
my $b_empty = sql(q{SELECT 'SELECT count(*) = 0 FROM pg_toast.pg_prolly_log_' || 's.b'::regclass::oid});
ok($node->poll_query_until('postgres', $b_empty), "b's log is folded");

# Its trim took the writer's rows: nothing is left in t's log, though no
# fold after has t's log due.
ok($node->poll_query_until('postgres', $empty),
	'the log empties after the writer that committed as the fold began');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)}),
	'10', 'the writes are in the working set');

$node->stop;
done_testing();
