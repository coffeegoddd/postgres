# Copyright (c) 2026, PostgreSQL Global Development Group

# What capture's decoding finds, in commit order: begin, each change, truncate, messages, commit.
# The sink in use says what it is told in the log.  Which branch it is told is t/105's to check.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('decoding');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_min_messages = debug1\nlog_line_prefix = ''\nautovacuum = off\nmax_prepared_transactions = 5\nlogical_decoding_work_mem = 64kB\n");
$node->start;
my $on = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $use = "SET catalog_branch = $on;";
$node->safe_psql('postgres', "$use CREATE SCHEMA s VERSIONED");
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}), 'its worker runs');
ok($node->wait_for_log(qr/capture starts at [0-9A-F]+\/[0-9A-F]+/), 'the sink is told where it starts');
my ($rc, $out, $err) = $node->psql('postgres', q{SELECT pg_create_logical_replication_slot('mine', 'prolly_capture')});
like($err, qr/is for capture's background worker alone/, 'a session cannot drive the sink');
$node->safe_psql('postgres', "$use CREATE TABLE s.t (id integer PRIMARY KEY, v text); CREATE TABLE s.k (a integer)");
my $made = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$made'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
  or die 'capture did not reach the tables';

# Runs a statement, waits until capture has decoded what it wrote, and gives what it was told.
sub told
{
	my ($sql) = @_;
	my $at = -s $node->logfile;
	$node->safe_psql('postgres', "$use $sql");
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres',
		qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die "capture did not reach $to";
	my $log = slurp_file($node->logfile, $at);
	return join("\n", $log =~ /capture: (?:branch \d+: )?(.*)$/mg);
}

is(told(q{CREATE TABLE s.d (a integer)}), "begin\ncommit", 'a transaction of nothing but DDL has no changes');
is(told(q{INSERT INTO s.t VALUES (1, 'a')}), "begin\ninsert t old=no new=yes\ncommit", 'an insert');
is(told(q{UPDATE s.t SET v = 'b' WHERE id = 1}), "begin\nupdate t old=no new=yes\ncommit", 'an update');
is(told(q{UPDATE s.t SET id = 2 WHERE id = 1}), "begin\nupdate t old=yes new=yes\ncommit", 'a key updated says the old key');
is(told(q{DELETE FROM s.t WHERE id = 2}), "begin\ndelete t old=yes new=no\ncommit", 'a delete');
is(told(q{ALTER TABLE s.k REPLICA IDENTITY FULL; INSERT INTO s.k VALUES (1); UPDATE s.k SET a = 2}),
	"begin\ncommit\nbegin\ninsert k old=no new=yes\ncommit\nbegin\nupdate k old=yes new=yes\ncommit", 'replica identity full says the old row');
is(told(q{TRUNCATE s.k}), "begin\ntruncate k restart=no\ncommit", 'a truncate');
is(told(q{TRUNCATE s.t, s.k RESTART IDENTITY}), "begin\ntruncate t restart=yes\ntruncate k restart=yes\ncommit", 'a truncate of two');
# (the message comes after the abort in the WAL, so this waits until it has been read)
is(told(q{BEGIN; INSERT INTO s.t VALUES (10, 'x'); ROLLBACK; SELECT pg_logical_emit_message(false, 'end', 'x', true)}),
	'message end size=1 transactional=no', 'what is rolled back is not seen');
is(told(q{BEGIN; INSERT INTO s.t VALUES (11, 'x'); SAVEPOINT p; INSERT INTO s.t VALUES (12, 'y'); ROLLBACK TO p; INSERT INTO s.t VALUES (13, 'z'); COMMIT}),
	"begin\ninsert t old=no new=yes\ninsert t old=no new=yes\ncommit", 'nor what a savepoint rolled back');
is(told(q{SELECT pg_logical_emit_message(false, 'p', 'now', true)}), 'message p size=3 transactional=no', 'a message at once');
is(told(q{BEGIN; SELECT pg_logical_emit_message(true, 'p', 'then'); INSERT INTO s.t VALUES (20, 'x'); COMMIT}),
	"begin\nmessage p size=4 transactional=yes\ninsert t old=no new=yes\ncommit", 'a message in its transaction');

is(told(q{BEGIN; INSERT INTO s.t VALUES (50, 'p'); PREPARE TRANSACTION 'one'; SELECT pg_logical_emit_message(false, 'end', 'x', true)}),
	'message end size=1 transactional=no', 'a prepared transaction is not told');
is(told(q{COMMIT PREPARED 'one'}), "begin\ninsert t old=no new=yes\ncommit", 'until it commits');
is(told(q{BEGIN; INSERT INTO s.t VALUES (51, 'p'); PREPARE TRANSACTION 'two'; ROLLBACK PREPARED 'two'; SELECT pg_logical_emit_message(false, 'end', 'x', true)}),
	'message end size=1 transactional=no', 'nor one rolled back');
is(told(q{CREATE TABLE s.c AS SELECT 1 AS a}), "begin\ninsert c old=no new=yes\ncommit", 'a table made from a query');
is(told(q{BEGIN; INSERT INTO s.k VALUES (1); ALTER TABLE s.k ALTER COLUMN a TYPE bigint; INSERT INTO s.k VALUES (2); COMMIT}),
	"begin\ninsert k old=no new=yes\ninsert k old=no new=yes\ncommit", 'a rewrite tells only the rows around it');
is(told(q{INSERT INTO s.t VALUES (20, 'y') ON CONFLICT (id) DO UPDATE SET v = excluded.v}), "begin\nupdate t old=no new=yes\ncommit", 'on conflict, an update');
is(told(q{INSERT INTO s.t SELECT 1000 + g, 'x' FROM generate_series(1, 2000) g}), join("\n", 'begin', ('insert t old=no new=yes') x 2000, 'commit'),
	'more than fits in memory');
ok($node->poll_query_until('postgres', qq{SELECT spill_txns > 0 FROM pg_stat_replication_slots WHERE slot_name = '$slot'}), 'and it did not');

# commit order, not the order of writing
my $a = $node->background_psql('postgres');
$a->query_safe("$use BEGIN; INSERT INTO s.t VALUES (30, 'first written')");
my $at = -s $node->logfile;
$node->safe_psql('postgres', "$use INSERT INTO s.k VALUES (31)");
$a->query_safe('COMMIT');
$a->quit;
my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'});
is(join("\n", slurp_file($node->logfile, $at) =~ /capture: (?:branch \d+: )?(.*)$/mg),
	"begin\ninsert k old=no new=yes\ncommit\nbegin\ninsert t old=no new=yes\ncommit", 'transactions come in commit order');

# a long transaction leaves the slot where it can still be read from
my $held = $node->background_psql('postgres');
$held->query_safe("$use BEGIN; INSERT INTO s.t VALUES (40, 'open')");
my $mid = told(q{INSERT INTO s.k VALUES (41)});
is($mid, "begin\ninsert k old=no new=yes\ncommit", 'another transaction goes by one open');

# killed, it starts again from where it was told it could
my $pid = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $pid})
  or die 'no worker came back';
$at = -s $node->logfile;
$held->query_safe('COMMIT');
$held->quit;
$to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'});
is(join("\n", slurp_file($node->logfile, $at) =~ /capture: (?:branch \d+: )?(.*)$/mg), "begin\ninsert t old=no new=yes\ncommit",
	'a transaction open across a restart of the worker is seen once it commits, and nothing twice');

# Leaves an open transaction with a record only part flushed, that capture has had time to look at.
# (A running_xacts record can flush the rest, so that needs another try.)
sub part_flushed
{
	my ($session) = @_;
	for (1 .. 5)
	{
		$session->query_safe(q{BEGIN; SELECT pg_logical_emit_message(true, 'p', repeat('x', 160000))});
		sleep(3);
		return 1
		  if $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn() < pg_current_wal_insert_lsn()') eq 't';
		$session->query_safe('ROLLBACK');
	}
	return 0;
}

# a record part flushed is read whole once the rest is
{
	my $whole = $node->background_psql('postgres');
	my $from = -s $node->logfile;
	ok(part_flushed($whole), 'the record is only part flushed');
	$whole->query_safe('COMMIT');
	$whole->quit;
	my $end = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$end'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die 'capture did not reach the commit';
	is(join("\n", slurp_file($node->logfile, $from) =~ /capture: (?:branch \d+: )?(.*)$/mg), "begin\nmessage p size=160000 transactional=yes\ncommit", 'told once, whole');
}

# with the rest never coming, the worker waits in its loop, and goes with the postmaster
my $part = $node->background_psql('postgres');
my $wpid = $node->safe_psql('postgres', qq{SELECT active_pid FROM pg_replication_slots WHERE slot_name = '$slot'});
ok(part_flushed($part), 'the record is only part flushed');
$node->kill9;
my $gone = 0;
for (1 .. 100)
{
	$gone = !kill(0, $wpid);
	last if $gone;
	select(undef, undef, undef, 0.1);
}
ok($gone, 'the worker does not outlive the postmaster');
kill('TERM', $wpid) unless $gone;
eval { $part->quit };

done_testing();
