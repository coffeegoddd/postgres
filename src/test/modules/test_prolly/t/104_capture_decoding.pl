# Copyright (c) 2026, PostgreSQL Global Development Group

# What capture's decoding finds, in commit order: begin, each change, truncate, messages, commit.
# The sink in use says what it is told in the log.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('decoding');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_min_messages = debug1\nlog_line_prefix = ''\n");
$node->start;
my $on = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $use = "SET catalog_branch = $on;";
$node->safe_psql('postgres', "$use CREATE SCHEMA s VERSIONED");
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}), 'its worker runs');
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
	return join("\n", $log =~ /capture: (.*)$/mg);
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
is(told(q{BEGIN; INSERT INTO s.t VALUES (10, 'x'); ROLLBACK; SELECT 1}), '', 'what is rolled back is not seen');
is(told(q{BEGIN; INSERT INTO s.t VALUES (11, 'x'); SAVEPOINT p; INSERT INTO s.t VALUES (12, 'y'); ROLLBACK TO p; INSERT INTO s.t VALUES (13, 'z'); COMMIT}),
	"begin\ninsert t old=no new=yes\ninsert t old=no new=yes\ncommit", 'nor what a savepoint rolled back');
is(told(q{SELECT pg_logical_emit_message(false, 'p', 'now', true)}), 'message p now transactional=no', 'a message at once');
is(told(q{BEGIN; SELECT pg_logical_emit_message(true, 'p', 'then'); INSERT INTO s.t VALUES (20, 'x'); COMMIT}),
	"begin\nmessage p then transactional=yes\ninsert t old=no new=yes\ncommit", 'a message in its transaction');

# commit order, not the order of writing
my $a = $node->background_psql('postgres');
$a->query_safe("$use BEGIN; INSERT INTO s.t VALUES (30, 'first written')");
my $at = -s $node->logfile;
$node->safe_psql('postgres', "$use INSERT INTO s.k VALUES (31)");
$a->query_safe('COMMIT');
$a->quit;
my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'});
is(join("\n", slurp_file($node->logfile, $at) =~ /capture: (.*)$/mg),
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
is(join("\n", slurp_file($node->logfile, $at) =~ /capture: (.*)$/mg), "begin\ninsert t old=no new=yes\ncommit",
	'a transaction open across a restart of the worker is seen once it commits, and nothing twice');

$node->stop;
done_testing();
