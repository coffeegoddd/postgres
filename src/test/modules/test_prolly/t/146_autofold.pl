# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum folds the working set once a versioned table's change log
# holds more than prolly.autofold_threshold of the table's size: the log is
# trimmed, and the next commit reads none of it, its result as a full
# fold's.  A threshold of 0, set for the database, turns it off.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('autofold');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum_naptime = 1
prolly.autofold_threshold = 0.05
log_min_messages = debug1
# (the worker off: the folds the test waits for are autovacuum's)
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Autovacuum's folds logged since offset.
sub folds_since
{
	my $log = slurp_file($node->logfile, $_[0]);
	return scalar(() = $log =~ /automatic fold of database "postgres"/g);
}

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA af VERSIONED;
	CREATE TABLE af.t (id int PRIMARY KEY, v text);
	INSERT INTO af.t SELECT g, repeat('v', 40) FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'loaded');
});
my $log = 'pg_toast.pg_prolly_log_' . sql(q{SELECT 'af.t'::regclass::oid});

# A sixth of the rows changed, a statement a transaction: past 5%.
sql(qq{UPDATE af.t SET v = repeat('w', 40) WHERE id = $_}) for 1 .. 20;
sql(q{UPDATE af.t SET v = repeat('x', 40) WHERE id % 6 = 0});
ok(sql("SELECT count(*) > 0 FROM $log") eq 't', 'the writes are in the log');
ok($node->poll_query_until('postgres', "SELECT count(*) = 0 FROM $log"),
	'autovacuum folded the working set, the log trimmed');

sql(q{SELECT pg_stat_reset_shared('prolly')});
is(sql(q{
	SELECT length(dolt_commit('-am', 'changed')::text);
	SELECT pg_stat_force_next_flush();
}), "42\n", 'committed');
is(sql(q{
	SELECT sum(value) FILTER (WHERE name = 'log_pages_read') || '|' ||
		sum(value) FILTER (WHERE name = 'fold_full_tables')
	FROM pg_stat_prolly
}), '0|0', 'the commit read no log row, no table in full');
is(sql(q{SELECT bool_and(matches) FROM dolt_verify_folds()}), 't',
	'its folds are as full ones');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::af.t)}),
	'3350', 'the commit has every change');

# Again, one transaction's writes (a log row a page of them), beside the
# dead rows the last fold left.
sql(q{UPDATE af.t SET v = repeat('y', 40) WHERE id % 5 = 0});
ok($node->poll_query_until('postgres', "SELECT count(*) = 0 FROM $log"),
	'folded again');

# A snapshot held from before the writes: a fold can't trim them while it
# is, so they don't make the table due (else autovacuum would fold at
# every pass); once it ends, they're folded.
my $rr = $node->background_psql('postgres');
$rr->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM af.t});
sql(q{UPDATE af.t SET v = repeat('h', 40) WHERE id % 5 = 0});
my $offset = -s $node->logfile;
sleep(5);
ok(folds_since($offset) <= 1, 'held back: not folded at every pass');
$rr->query_safe('COMMIT');
$rr->quit;
ok($node->poll_query_until('postgres', "SELECT count(*) = 0 FROM $log"),
	'folded once the snapshot ended');

# A table with autovacuum_enabled = false doesn't make the working set due.
sql(q{ALTER TABLE af.t SET (autovacuum_enabled = false)});
sql(q{UPDATE af.t SET v = repeat('o', 40) WHERE id % 5 = 0});
sleep(5);
ok(sql("SELECT count(*) > 0 FROM $log") eq 't',
	'autovacuum_enabled = false: not folded');
sql(q{ALTER TABLE af.t RESET (autovacuum_enabled)});
ok($node->poll_query_until('postgres', "SELECT count(*) = 0 FROM $log"),
	'folded once enabled again');

# Off for the database (autovacuum's workers take its settings): the log
# keeps the writes.
sql(q{ALTER DATABASE postgres SET prolly.autofold_threshold = 0});
sql(q{UPDATE af.t SET v = repeat('z', 40) WHERE id % 5 = 0});
sleep(5);
ok(sql("SELECT count(*) > 0 FROM $log") eq 't', 'off: not folded');

$node->stop;
done_testing();
