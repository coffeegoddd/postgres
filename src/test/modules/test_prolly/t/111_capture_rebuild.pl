# Copyright (c) 2026, PostgreSQL Global Development Group

# A new slot begins with a row map of every table with a key, read under the snapshot of its first
# position: that and what's told after it are the whole of a table, whatever's happening meanwhile.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my ($node, $slot);

sub new_node
{
	my ($name) = @_;
	$node = PostgreSQL::Test::Cluster->new($name);
	$node->init(allows_streaming => 'logical');
	$node->append_conf('postgresql.conf', "log_min_messages = debug1\nlog_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\n");
	$node->start;
	$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
	$slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
	return $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
}

sub began
{
	$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0})
	  or die 'capture did not begin';
	$node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die 'no worker';
}

sub same_map
{
	my ($on, $table, $what) = @_;
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres',
		qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die "capture did not reach $to";
	is( $node->safe_psql('postgres',
			"$on SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.$table'::regclass::oid"),
		$node->safe_psql('postgres', "$on SELECT test_ws_rows_hash('s', '$table')"),
		$what);
}

# statements one after another, with no waiting for capture to get going
my $b = new_node('script');
my $on = "SET catalog_branch = $b;";
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
$node->safe_psql('postgres', "$on CREATE TABLE s.t (id integer PRIMARY KEY, v text)");
$node->safe_psql('postgres', "$on INSERT INTO s.t SELECT g, 'v' || g FROM generate_series(1, 3000) g");
$node->safe_psql('postgres', "$on UPDATE s.t SET v = 'changed' WHERE id % 3 = 0");
$node->safe_psql('postgres', "$on DELETE FROM s.t WHERE id % 5 = 0");
$node->safe_psql('postgres', "$on CREATE TABLE s.big (id integer PRIMARY KEY, v text); ALTER TABLE s.big ALTER v SET STORAGE EXTERNAL; INSERT INTO s.big VALUES (1, repeat('x', 100000))");
began();
same_map($on, 't', 'a script that made a schema and wrote at once');
same_map($on, 'big', 'a large value');
$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (9001, 'after'); UPDATE s.t SET v = 'then' WHERE id = 1; DELETE FROM s.t WHERE id = 2");
same_map($on, 't', 'and what comes after');

# a worker dying before the slot's kept leaves no slot; the next begins again
SKIP:
{
	skip 'injection points not supported by this build', 3 if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
	$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
	$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (9002, 'while off')");
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-slot-snapshot', 'wait')});
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
	$node->wait_for_event('capture worker', 'capture-slot-snapshot');
	is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_history.capture_positions WHERE branch = 0}), '0', 'what says capture began goes before a new slot');
	my $dying = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-snapshot')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($dying)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $dying})
	  or die 'no worker came back';
	began();
	same_map($on, 't', 'a worker dying with its slot not yet kept, and the next beginning again');
	$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (9003, 'after')");
	same_map($on, 't', 'and what comes after');

	# what happens between the slot's snapshot and reading under it is told, and comes after
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
	$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-slot-snapshot', 'wait')});
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
	$node->wait_for_event('capture worker', 'capture-slot-snapshot');
	$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (9004, 'in the gap'); UPDATE s.t SET v = 'gap' WHERE id = 1; DELETE FROM s.t WHERE id = 3");
	$node->safe_psql('postgres', "$on CREATE TABLE s.made_in_gap (id integer PRIMARY KEY, v text); INSERT INTO s.made_in_gap VALUES (1, 'x')");
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-snapshot')});
	$node->safe_psql('postgres', q{SELECT injection_points_wakeup('capture-slot-snapshot')});
	began();
	same_map($on, 't', 'changes between the snapshot and the read');
	same_map($on, 'made_in_gap', 'a table made between them');

	# and a statement telling a table's rows, between them
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
	$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-slot-snapshot', 'wait')});
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
	$node->wait_for_event('capture worker', 'capture-slot-snapshot');
	$node->safe_psql('postgres', "$on ALTER TABLE s.made_in_gap ALTER COLUMN v TYPE text USING upper(v); ALTER TABLE s.made_in_gap ADD COLUMN w integer DEFAULT 1");
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-snapshot')});
	$node->safe_psql('postgres', q{SELECT injection_points_wakeup('capture-slot-snapshot')});
	began();
	same_map($on, 'made_in_gap', 'a table rewritten between them');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
}

$node->stop;

# transactions open when capture begins: it waits for them, and has them whole
$b = new_node('open_txn');
$on = "SET catalog_branch = $b;";
my $open = $node->background_psql('postgres');
$open->query_safe(q{BEGIN; CREATE SCHEMA unversioned_open; CREATE TABLE unversioned_open.u (id integer PRIMARY KEY); INSERT INTO unversioned_open.u VALUES (1)});
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (id integer PRIMARY KEY); INSERT INTO s.t VALUES (1)");
my $writer = $node->background_psql('postgres');
$writer->query_safe("$on BEGIN; INSERT INTO s.t VALUES (2)");
$open->query_safe('COMMIT');
$open->quit;
$writer->query_safe('COMMIT');
$writer->quit;
began();
same_map($on, 't', 'transactions open as capture began');

# a new slot forgets the maps of tables that went while there was none
$node->safe_psql('postgres', "$on CREATE TABLE s.dropped_while_off (id integer PRIMARY KEY); INSERT INTO s.dropped_while_off VALUES (1)");
same_map($on, 'dropped_while_off', 'a table to drop');
my $gone_id = $node->safe_psql('postgres', "$on SELECT 's.dropped_while_off'::regclass::oid");

# many tables on two branches, and a table with no key, which only stops it once written
my $b2 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on2 = "SET catalog_branch = $b2;";
$node->safe_psql('postgres', "$on2 CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (id integer PRIMARY KEY); INSERT INTO s.t VALUES (20)");
$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
$node->safe_psql('postgres', "$on INSERT INTO s.t VALUES (3)");
$node->safe_psql('postgres', "$on2 INSERT INTO s.t VALUES (21)");
$node->safe_psql('postgres', "$on CREATE TABLE s.nokey (a integer); INSERT INTO s.nokey VALUES (1)");
$node->safe_psql('postgres', "$on DROP TABLE s.dropped_while_off");
$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
began();
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_history.pointers WHERE name = 'capture/map/$gone_id'"), '0', 'a new slot forgets the map of a table gone');

# it records each table's fingerprint, so that the first catalog change tells only what it changed
my $tables = $node->safe_psql('postgres', q{SET catalog_branch_filter = off; SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspbranch <> 0 AND c.relkind = 'r'});
is($node->safe_psql('postgres', 'SELECT count(*) FROM pg_history.capture_fingerprints'), $tables, 'each table has its fingerprint');
my $recorded = q{SELECT string_agg(relid || ':' || xmin, ',' ORDER BY relid) FROM pg_history.capture_fingerprints};
my $before = $node->safe_psql('postgres', $recorded);
$node->safe_psql('postgres', 'CREATE TEMP TABLE first_catalog_change (a integer)');
is($node->safe_psql('postgres', $recorded), $before, 'the first catalog change records no table anew');
same_map($on, 't', 'rebuilt on one branch');
same_map($on2, 't', 'and another');
is($node->safe_psql('postgres', "$on SELECT count(*) FROM pg_history.pointers WHERE name = 'capture/map/' || 's.nokey'::regclass::oid"), '0', 'a table with no key has no map');
my $from = -s $node->logfile;
$node->safe_psql('postgres', "$on INSERT INTO s.nokey VALUES (2)");
ok($node->wait_for_log(qr/which has no primary key/, $from), 'and stops capture only once it is written');
$node->stop;

# tables whose rows don't lie in key order don't cost what every tree between would
$b = new_node('order');
$on = "SET catalog_branch = $b;";
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
began();
$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
$node->safe_psql('postgres', "$on CREATE TABLE s.t (id integer PRIMARY KEY, v text); INSERT INTO s.t SELECT g, repeat('x', 40) FROM generate_series(1, 30000) g ORDER BY random()");
my $size = q{SELECT sum(pg_relation_size(c.oid)) FROM pg_class c WHERE c.relnamespace = 'pg_history'::regnamespace AND c.relname ~ '^chunks'};
my $size_at = $node->safe_psql('postgres', $size);
$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
began();
same_map($on, 't', 'a table in random order');
my $store = $node->safe_psql('postgres', $size) - $size_at;
my $table = $node->safe_psql('postgres', "$on SELECT pg_relation_size('s.t')");
ok($store < 3 * $table, "the store grew by no more than a few times the table ($store bytes for $table)");
$node->stop;

# a table locked after the slot's snapshot by a transaction yet to commit: capture gives way, and
# it's not the commit that fails
SKIP:
{
	skip 'injection points not supported by this build', 8 if $ENV{enable_injection_points} ne 'yes';
	$b = new_node('locked');
	$on = "SET catalog_branch = $b;";
	$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (id integer PRIMARY KEY, a integer); INSERT INTO s.t VALUES (1, 1)");
	began();
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
	$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-slot-snapshot', 'wait')});
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
	$node->wait_for_event('capture worker', 'capture-slot-snapshot');
	my $holder = $node->background_psql('postgres');
	$holder->query_safe("$on BEGIN; ALTER TABLE s.t ALTER COLUMN a TYPE bigint USING a + 1");
	my $locked_from = -s $node->logfile;
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-snapshot')});
	$node->safe_psql('postgres', q{SELECT injection_points_wakeup('capture-slot-snapshot')});
	ok($node->wait_for_log(qr/capture can't begin yet, as relation \d+ is locked/, $locked_from), 'a worker gives way to a table locked');
	$holder->query_safe('COMMIT');
	$holder->quit;
	began();
	same_map($on, 't', 'and begins once it is not');
	unlike(slurp_file($node->logfile), qr/deadlock detected/, 'with no deadlock');

	# during the read, DDL on a table waits for the table, and any other catalog commit its turn, not
	# for a row the read is rewriting; neither makes the read give way
	$node->safe_psql('postgres', "$on CREATE TABLE s.other_t (id integer PRIMARY KEY, a integer); INSERT INTO s.other_t VALUES (1, 1)");
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
	$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-slot-locked', 'wait')});
	$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
	$node->wait_for_event('capture worker', 'capture-slot-locked');
	my $read_from = -s $node->logfile;
	my $during = $node->background_psql('postgres');
	$during->query_until(qr/started/, "\\echo started\n$on ALTER TABLE s.t ALTER COLUMN a TYPE bigint USING a + 1;\n\\echo finished\n");
	ok( $node->poll_query_until('postgres',
			q{SELECT count(*) = 1 FROM pg_stat_activity WHERE query LIKE 'ALTER TABLE s.t%' AND wait_event_type = 'Lock' AND wait_event = 'relation'}),
		'DDL during the read waits for the table');
	my $other = $node->background_psql('postgres');
	$other->query_until(qr/started/, "\\echo started\nCREATE TEMP TABLE during_read (a integer);\n\\echo finished\n");
	ok( $node->poll_query_until('postgres',
			q{SELECT count(*) = 1 FROM pg_stat_activity WHERE query LIKE 'CREATE TEMP TABLE during_read%' AND wait_event_type = 'Lock' AND wait_event = 'object'}),
		'and any other catalog commit waits its turn');
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-slot-locked')});
	$node->safe_psql('postgres', q{SELECT injection_points_wakeup('capture-slot-locked')});
	$during->quit;
	$other->quit;
	began();
	unlike(slurp_file($node->logfile, $read_from), qr/capture can't begin yet/, 'the read didn\'t give way');
	is($node->safe_psql('postgres', "$on SELECT format_type(atttypid, NULL) FROM pg_attribute WHERE attrelid = 's.t'::regclass AND attname = 'a'"), 'bigint', 'the DDL went through');
	same_map($on, 't', 'and comes after it');
	$node->stop;
}

# a database's settings don't stop a worker with the whole cluster's WAL behind it
$b = new_node('settings');
$node->safe_psql('postgres', q{CREATE DATABASE settings_db; ALTER DATABASE settings_db SET default_transaction_read_only = on;
	ALTER DATABASE settings_db SET transaction_timeout = '1ms'; ALTER DATABASE settings_db SET statement_timeout = '1ms'});
my $s_slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'settings_db'});
my $open_db = q{SET default_transaction_read_only = off; SET transaction_timeout = 0; SET statement_timeout = 0;};
my $sb = $node->safe_psql('settings_db', qq{$open_db SELECT pg_branch_id_allocate()});
$sb = (split /\n/, $sb)[-1];
$node->safe_psql('settings_db', qq{$open_db SET catalog_branch = $sb; CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (id integer PRIMARY KEY); INSERT INTO s.t SELECT generate_series(1, 20000)});
# (the mark, and not the slot's being there, which it is while it's being made)
my $began_there = 0;
for (1 .. 300)
{
	my @lines = split /\n/, $node->safe_psql('settings_db', qq{$open_db SELECT count(*) FROM pg_history.capture_positions WHERE branch = 0});
	$began_there = $lines[-1] eq '1';
	last if $began_there;
	usleep(100_000);
}
ok($began_there, 'capture begins, whatever a database\'s settings');
$node->stop;

# capture's worker is a superuser: reading tables as it begins runs nothing of theirs, and a
# database's settings don't choose what its SQL means
new_node('booby');
$node->safe_psql('postgres', q{CREATE ROLE evil LOGIN; CREATE DATABASE evil_db OWNER evil});
$node->safe_psql('evil_db', 'CREATE EXTENSION test_prolly');
my $eb = $node->safe_psql('evil_db', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('evil_db', q{SET ROLE evil; CREATE SCHEMA evil;
	CREATE FUNCTION evil.do_it(boolean) RETURNS boolean LANGUAGE plpgsql VOLATILE AS $$
		BEGIN EXECUTE 'ALTER ROLE evil SUPERUSER'; RETURN true; EXCEPTION WHEN insufficient_privilege THEN RETURN true; END $$;
	CREATE FUNCTION evil.fold_me() RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT evil.do_it(true)';
	CREATE FUNCTION evil.oid_eq(oid, oid) RETURNS boolean LANGUAGE plpgsql AS $$
		BEGIN PERFORM evil.do_it(true); RETURN pg_catalog.oideq($1, $2); END $$;
	CREATE OPERATOR evil.= (LEFTARG = oid, RIGHTARG = oid, FUNCTION = evil.oid_eq);
	CREATE FUNCTION evil.int_eq(integer, integer) RETURNS boolean LANGUAGE plpgsql AS $$
		BEGIN PERFORM evil.do_it(true); RETURN pg_catalog.int4eq($1, $2); END $$;
	CREATE OPERATOR evil.= (LEFTARG = integer, RIGHTARG = integer, FUNCTION = evil.int_eq);
	ALTER DATABASE evil_db SET search_path = evil, pg_catalog});
$node->safe_psql('evil_db', qq{SET catalog_branch = $eb; SET ROLE evil; SET search_path = pg_catalog, evil; CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id integer PRIMARY KEY, a integer);
	INSERT INTO s.t VALUES (1, 1);
	CREATE INDEX t_partial ON s.t (a) WHERE evil.fold_me();
	CREATE INDEX t_expr ON s.t ((evil.fold_me()))});
# (with operators named: the database's search path would have this session run evil's)
$node->poll_query_until('evil_db',
	q{SELECT count(*) OPERATOR(pg_catalog.=) 1 FROM pg_history.capture_positions WHERE branch OPERATOR(pg_catalog.=) 0})
  or die 'capture did not begin';
$node->safe_psql('evil_db', qq{SET search_path = pg_catalog; SET ROLE evil; SET catalog_branch = $eb; INSERT INTO s.t VALUES (2, 2)});
my $evil_slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'evil_db'});
my $evil_to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$evil_to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$evil_slot'})
  or die 'capture did not reach it';
is($node->safe_psql('postgres', q{SELECT rolsuper FROM pg_roles WHERE rolname = 'evil'}), 'f', 'a database\'s owner can\'t make itself a superuser through capture');
$node->stop;

done_testing();
