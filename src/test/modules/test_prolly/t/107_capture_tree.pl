# Copyright (c) 2026, PostgreSQL Global Development Group

# Capture applying what it decodes: a table's row map is, to the byte, the one layer 4 makes by
# reading the table.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

my $node = PostgreSQL::Test::Cluster->new('tree');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf',
	"log_min_messages = debug1\nlog_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\nlogical_decoding_work_mem = 64kB\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $b1 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b2 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on1 = "SET catalog_branch = $b1;";
my $on2 = "SET catalog_branch = $b2;";
$node->safe_psql('postgres', "$on1 CREATE SCHEMA s VERSIONED");
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0})
  or die 'no worker';

sub caught_up
{
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres',
		qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die "capture did not reach $to";
}

# Whether capture's row map of a table is the one layer 4 makes of it now.
sub same_map
{
	my ($on, $table, $what) = @_;
	caught_up();
	my $read = $node->safe_psql('postgres', "$on SELECT test_ws_rows_hash('s', '$table')");
	my $map = $node->safe_psql('postgres',
		"$on SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.$table'::regclass::oid");
	is($map, $read, $what);
}

$node->safe_psql('postgres', "$on1 CREATE TABLE s.t (id integer PRIMARY KEY, v text)");
$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (1, 'one')");
same_map($on1, 't', 'an insert');
$node->safe_psql('postgres', "$on1 UPDATE s.t SET v = 'uno' WHERE id = 1");
same_map($on1, 't', 'an update');
$node->safe_psql('postgres', "$on1 UPDATE s.t SET id = 2 WHERE id = 1");
same_map($on1, 't', 'a key updated');
$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (3, NULL), (4, 'x')");
same_map($on1, 't', 'a null');
$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id = 3");
same_map($on1, 't', 'a delete');
$node->safe_psql('postgres', "$on1 BEGIN; INSERT INTO s.t VALUES (10, 'a'); UPDATE s.t SET v = 'b' WHERE id = 10; UPDATE s.t SET id = 11 WHERE id = 10; DELETE FROM s.t WHERE id = 11; INSERT INTO s.t VALUES (12, 'c'); COMMIT");
same_map($on1, 't', 'one key changed over and over in a transaction');
$node->safe_psql('postgres', "$on1 BEGIN; INSERT INTO s.t VALUES (20, 'gone'); ROLLBACK");
same_map($on1, 't', 'what is rolled back');
$node->safe_psql('postgres', "$on1 BEGIN; UPDATE s.t SET id = 30 WHERE id = 2; UPDATE s.t SET id = 2 WHERE id = 4; COMMIT");
same_map($on1, 't', 'a key taken by another row');
$node->safe_psql('postgres', "$on1 INSERT INTO s.t SELECT 1000 + g, 'x' || g FROM generate_series(1, 3000) g");
same_map($on1, 't', 'enough rows for several chunks, spilled in decoding');
$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id % 2 = 0");
same_map($on1, 't', 'many deleted');
$node->safe_psql('postgres', "$on1 DELETE FROM s.t");
same_map($on1, 't', 'none left');

# values are as layer 4 prints them, whatever the session's settings
$node->safe_psql('postgres',
	"$on1 CREATE TYPE s.mood AS ENUM ('sad', 'ok'); CREATE TABLE s.vals (id integer PRIMARY KEY, a timestamptz, b float8, c numeric, d boolean, e bytea, f integer[], g inet, h s.mood, i interval, j jsonb, k date, l char(3), gen integer GENERATED ALWAYS AS (id + 1) STORED, virt integer GENERATED ALWAYS AS (id + 2) VIRTUAL)");
$node->safe_psql('postgres',
	qq{$on1 SET TimeZone = 'Asia/Tokyo'; SET DateStyle = 'German'; SET IntervalStyle = 'sql_standard'; SET extra_float_digits = -5; SET bytea_output = 'escape';
	INSERT INTO s.vals VALUES (1, '2026-01-02 03:04:05+00', 0.1, 1.50, true, '\\x00ff', '{1,NULL,3}', '10.0.0.1/8', 'ok', '1 day 2 hours', '{"b": 1, "a": 2}', '2026-11-06', 'a')});
same_map($on1, 'vals', 'every kind of value, under other settings');

# a value kept out of line, and one in a tree of its own
$node->safe_psql('postgres', "$on1 CREATE TABLE s.big (id integer PRIMARY KEY, v text); ALTER TABLE s.big ALTER v SET STORAGE EXTERNAL");
$node->safe_psql('postgres', "$on1 INSERT INTO s.big VALUES (1, repeat('x', 100000)), (2, repeat('y', 100))");
same_map($on1, 'big', 'large values');
$node->safe_psql('postgres', "$on1 DELETE FROM s.big WHERE id = 1");
same_map($on1, 'big', 'one gone');

# a key of several columns, in another order than the table's
$node->safe_psql('postgres', "$on1 CREATE TABLE s.k (a integer, b integer, c text, d integer, PRIMARY KEY (d, a))");
$node->safe_psql('postgres', "$on1 INSERT INTO s.k VALUES (1, 2, 'x', 3), (4, 5, 'y', 6); UPDATE s.k SET c = 'z' WHERE a = 1; UPDATE s.k SET d = 9 WHERE a = 4");
same_map($on1, 'k', 'a key out of order');

# two branches, the same names, their own rows; one transaction on both
$node->safe_psql('postgres', "$on2 CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (id integer PRIMARY KEY, v text); CREATE TABLE s.only2 (id integer PRIMARY KEY)");
$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (1, 'b1')");
$node->safe_psql('postgres', "$on2 INSERT INTO s.t VALUES (1, 'b2'), (2, 'b2')");
same_map($on1, 't', 'the first branch\'s rows');
same_map($on2, 't', 'the second\'s');
$node->safe_psql('postgres', "BEGIN; SET LOCAL catalog_branch = $b1; UPDATE s.t SET v = 'both'; SET LOCAL catalog_branch = $b2; UPDATE s.t SET v = 'both'; COMMIT");
same_map($on1, 't', 'one transaction, the first branch');
same_map($on2, 't', 'one transaction, the second');

# transactions apply in the order they committed, not began: this one began first, and wrote a key
# after the other had committed it
my $first = $node->background_psql('postgres');
$first->query_safe("$on1 BEGIN; UPDATE s.t SET v = 'began first' WHERE id = 1");
$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (40, 'committed first')");
$first->query_safe("UPDATE s.t SET v = 'committed last' WHERE id = 40; COMMIT");
$first->quit;
is($node->safe_psql('postgres', "$on1 SELECT v FROM s.t WHERE id = 40"), 'committed last', 'the later commit has the row');
same_map($on1, 't', 'transactions in commit order');

# shared schemas have no row map, and each branch knows how far it is
$node->safe_psql('postgres', 'CREATE SCHEMA pub; CREATE TABLE pub.u (id integer PRIMARY KEY); INSERT INTO pub.u VALUES (1)');
caught_up();
my $maps = $node->safe_psql('postgres', q{SELECT count(*) FROM pg_history.pointers WHERE name = 'capture/map/' || 'pub.u'::regclass::oid});
is($maps, '0', 'a shared table has no row map');
is($node->safe_psql('postgres', "SELECT array_agg(branch ORDER BY branch) = ARRAY[0, $b1, $b2] FROM pg_history.capture_positions"), 't', 'each branch has a position');

# what a worker writes it then decodes, and that mustn't set it writing again
caught_up();
my $from = -s $node->logfile;
$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (45, 'one more')");
caught_up();
caught_up();
my @applied = slurp_file($node->logfile, $from) =~ /capture tree: applied/g;
is(scalar(@applied), 1, 'its own writes give it nothing to apply');

# a worker stopped and started goes on from where it was
my $pid = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (41, 'while it was stopped')");
$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $pid})
  or die 'no worker came back';
same_map($on1, 't', 'after a restart');

# stopped having applied, and before saying so, it is told again and passes over what is done
SKIP:
{
	skip 'injection points not supported by this build', 3 if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	caught_up();
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-applied', 'wait')});
	$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (42, 'applied, not confirmed')");
	$node->wait_for_event('capture worker', 'capture-applied');
	my $held = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	my $log = -s $node->logfile;
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applied')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($held)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $held})
	  or die 'no worker came back';
	same_map($on1, 't', 'a transaction applied and told again');
	like(slurp_file($node->logfile, $log), qr/capture tree: passed over a change/, 'and passed over');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
	same_map($on1, 't', 'and on it goes');
}

# what's gone is let go of when a worker starts, and capture's state doesn't go in a dump
$node->safe_psql('postgres', "$on1 CREATE TABLE s.dropped (id integer PRIMARY KEY); INSERT INTO s.dropped VALUES (1)");
caught_up();
my $dropped = $node->safe_psql('postgres', "$on1 SELECT 's.dropped'::regclass::oid");
$node->safe_psql('postgres', "$on1 DROP TABLE s.dropped");
my $b3 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('postgres', "SET catalog_branch = $b3; CREATE SCHEMA gone VERSIONED; CREATE TABLE gone.x (id integer PRIMARY KEY); INSERT INTO gone.x VALUES (1)");
caught_up();
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_history.capture_positions WHERE branch = $b3"), '1', 'a third branch has a position');
$node->safe_psql('postgres', "SET catalog_branch = $b3; DROP SCHEMA gone CASCADE");
caught_up();
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_history.pointers WHERE name = 'capture/map/$dropped'"), '1', 'a dropped table\'s map, until a worker starts');
$pid = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $pid})
  or die 'no worker came back';
ok($node->poll_query_until('postgres', "SELECT count(*) = 0 FROM pg_history.pointers WHERE name = 'capture/map/$dropped'"), 'then it goes');
ok($node->poll_query_until('postgres', "SELECT count(*) = 0 FROM pg_history.capture_positions WHERE branch = $b3"), 'and a branch\'s position');
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_history.capture_tables WHERE relid = $dropped"), '0', 'and the table\'s record');
my $dump = '';
IPC::Run::run([ 'pg_dump', '--dbname', $node->connstr('postgres'), '--no-sync' ], '>', \$dump) or die 'pg_dump failed';
unlike($dump, qr/capture\/map\//, 'a dump has no map of capture\'s');
unlike($dump, qr/COPY pg_history\.capture_/, 'nor its positions or tables');

$node->stop;

# A node of its own for each of these, as each stops capture for good.
sub fresh_node
{
	my ($name, $conf) = @_;
	$node = PostgreSQL::Test::Cluster->new($name);
	$node->init(allows_streaming => 'logical');
	$node->append_conf('postgresql.conf', "log_min_messages = debug1\nlog_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\n" . ($conf // ''));
	$node->start;
	$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
	my $b = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
	$slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
	return "SET catalog_branch = $b;";
}

sub slot_active
{
	my ($db) = @_;
	$node->poll_query_until($db // 'postgres', q{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0})
	  or die 'no worker';
}

# a crash with what it applied not flushed, whatever synchronous_commit says, loses nothing
my $on = fresh_node('sync', "synchronous_commit = off\nwal_writer_delay = 10s\n");
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
slot_active();
$node->safe_psql('postgres', "$on SET synchronous_commit = on; CREATE TABLE s.t (id integer PRIMARY KEY, v text)");
# (a running-transactions record lets the slot's restart point move, which saves it to disk)
for my $i (1 .. 3)
{
	$node->safe_psql('postgres', "SET synchronous_commit = on; SELECT pg_log_standby_snapshot()");
	$node->safe_psql('postgres', "$on SET synchronous_commit = on; INSERT INTO s.t VALUES ($i, 'kept')");
	caught_up();
}
$node->stop('immediate');
$node->start;
same_map($on, 't', 'a crash just after applying');
$node->stop;

# a truncate changes the table's file, so its rows are told whole
$on = fresh_node('truncate');
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
slot_active();
$node->safe_psql('postgres', "$on CREATE TABLE s.t (id integer PRIMARY KEY); INSERT INTO s.t VALUES (1)");
caught_up();
$node->safe_psql('postgres', "$on TRUNCATE s.t");
same_map($on, 't', 'a truncate');
$node->safe_psql('postgres', "$on BEGIN; INSERT INTO s.t VALUES (2); TRUNCATE s.t; INSERT INTO s.t VALUES (3); COMMIT");
same_map($on, 't', 'and with rows around it, in one transaction');
$node->stop;

$on = fresh_node('unchanged_toast');
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
slot_active();
$node->safe_psql('postgres', "$on CREATE TABLE s.t (id integer PRIMARY KEY, n integer, v text); ALTER TABLE s.t ALTER v SET STORAGE EXTERNAL; INSERT INTO s.t VALUES (1, 1, repeat('x', 100000))");
caught_up();
$from = -s $node->logfile;
$node->safe_psql('postgres', "$on UPDATE s.t SET n = 2");
ok($node->wait_for_log(qr/was not given column "v" of "t"/, $from), 'an unchanged TOASTed value');
$node->stop;

# what's there when capture begins is in its maps
$on = fresh_node('schema_txn');
$node->safe_psql('postgres', "$on BEGIN; CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (id integer PRIMARY KEY); INSERT INTO s.t VALUES (1); COMMIT");
slot_active();
same_map($on, 't', 'a table made with the first schema');
$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (2)");
same_map($on, 't', 'and what comes after');
$node->stop;

$on = fresh_node('off_on');
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
slot_active();
$node->safe_psql('postgres', "$on CREATE TABLE s.t (id integer PRIMARY KEY); INSERT INTO s.t VALUES (1)");
caught_up();
$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (2)");
$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
$node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'no worker';
slot_active();
same_map($on, 't', 'capture off and on again, with rows meanwhile');
$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (3)");
same_map($on, 't', 'and what comes after');
$node->stop;

# a worker dying just after making a new slot leaves the next one knowing it's new
SKIP:
{
	skip 'injection points not supported by this build', 1 if $ENV{enable_injection_points} ne 'yes';
	$on = fresh_node('slot_made');
	$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
	slot_active();
	$node->safe_psql('postgres', "$on CREATE TABLE s.t (id integer PRIMARY KEY); INSERT INTO s.t VALUES (1)");
	caught_up();
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
	$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
	$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (2)");
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-slot-made', 'wait')});
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
	$node->wait_for_event('capture worker', 'capture-slot-made');
	my $dying = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-made')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($dying)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $dying})
	  or die 'no worker came back';
	slot_active();
	same_map($on, 't', 'a worker dying with its new slot just made');
	$node->stop;
}

$on = fresh_node('template');
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
slot_active();
$node->safe_psql('postgres', "$on CREATE TABLE s.t (id integer PRIMARY KEY); INSERT INTO s.t VALUES (1)");
caught_up();
$node->safe_psql('postgres', 'SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type = \'capture worker\'');
$node->safe_psql('postgres', 'CREATE DATABASE other TEMPLATE postgres');
$slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'other'});
$node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'no worker';
slot_active('other');
$node->safe_psql('other', "$on INSERT INTO s.t VALUES (2)");
my $other_to = $node->safe_psql('other', 'SELECT pg_current_wal_flush_lsn()');
$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$other_to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
  or die 'capture of the copy did not reach it';
is( $node->safe_psql('other', "$on SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.t'::regclass::oid"),
	$node->safe_psql('other', "$on SELECT test_ws_rows_hash('s', 't')"),
	'a database copied from one with a map begins with what it holds');
$node->stop;

$on = fresh_node('turned_on', "capture_to_trees = off\n");
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
$node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'no worker';
$node->safe_psql('postgres', "$on CREATE TABLE s.t (id integer PRIMARY KEY); INSERT INTO s.t VALUES (1)");
caught_up();
$node->append_conf('postgresql.conf', "capture_to_trees = on\n");
$node->restart;
slot_active();
same_map($on, 't', 'capture to trees turned on with rows there, by a new slot');
$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (2)");
same_map($on, 't', 'and what comes after');
$node->stop;

# a layout changed with capture to trees off, and so nothing told: it stops, and stays stopped
$on = fresh_node('layout');
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
slot_active();
$node->safe_psql('postgres', "$on CREATE TABLE s.t (id integer PRIMARY KEY); INSERT INTO s.t VALUES (1)");
caught_up();
my $relid = $node->safe_psql('postgres', "$on SELECT 's.t'::regclass::oid");
$node->append_conf('postgresql.conf', "capture_to_trees = off\n");
$node->restart;
$node->safe_psql('postgres', "$on ALTER TABLE s.t ADD COLUMN later integer");
$node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'no worker';
caught_up();
$node->append_conf('postgresql.conf', "capture_to_trees = on\n");
$node->restart;
$from = -s $node->logfile;
$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (2, 2)");
ok($node->wait_for_log(qr/does not follow the layout of relation $relid changing/, $from), 'a layout changing and nothing told is an error');
$from = -s $node->logfile;
my $stopped = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'}) || 0;
$node->safe_psql('postgres', "SELECT pg_terminate_backend($stopped)") if $stopped;
ok($node->wait_for_log(qr/does not follow the layout of relation $relid changing/, $from), 'and it still is for the next worker');
$node->stop;

done_testing();
