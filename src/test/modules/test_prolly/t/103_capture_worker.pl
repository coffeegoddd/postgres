# Copyright (c) 2026, PostgreSQL Global Development Group

# One capture worker per database with a versioned schema, on one logical slot: started with its
# first versioned schema and with the server, gone with the last, and in nothing's way.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('capture');
$node->init(allows_streaming => 'logical');
$node->start;

my $db = $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
my $slot = "prolly_capture_$db";
my $on = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $use = "SET catalog_branch = $on;";

sub slots_of
{
	return $node->safe_psql('postgres',
		q{SELECT coalesce(string_agg(slot_name || ':' || active || ':' || plugin, ',' ORDER BY slot_name), '')
		  FROM pg_replication_slots WHERE slot_name LIKE 'prolly_capture_%'});
}

sub workers_of
{
	return $node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'capture worker'});
}

ok($node->poll_query_until('postgres',
	q{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture launcher'}), 'the launcher runs');

# what starts it, and only that
$node->safe_psql('postgres', q{CREATE SCHEMA plain; CREATE TABLE plain.t (a integer)});
$node->safe_psql('postgres', "$use BEGIN; CREATE SCHEMA gone VERSIONED; ROLLBACK;");
is(slots_of(), '', 'no slot for a database with no versioned schema, nor one rolled back');
is(workers_of(), '0', 'nor a worker');

$node->safe_psql('postgres', "$use CREATE SCHEMA s1 VERSIONED");
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}),
	'a first versioned schema starts its worker, on its slot');
is(slots_of(), "$slot:true:prolly_capture", 'on the plugin built in');

# one, however many schemas
$node->safe_psql('postgres', "$use CREATE SCHEMA s2 VERSIONED");
ok($node->poll_query_until('postgres',
	q{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker'}), 'one worker');
sleep(2);
is(workers_of(), '1', 'and still one after another schema');

# it moves the slot along
my $was = $node->safe_psql('postgres', qq{SELECT confirmed_flush_lsn FROM pg_replication_slots WHERE slot_name = '$slot'});
$node->safe_psql('postgres', "$use CREATE TABLE s1.t (a integer); INSERT INTO s1.t SELECT generate_series(1, 100)");
ok($node->poll_query_until('postgres',
	qq{SELECT confirmed_flush_lsn > '$was'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'}),
	'the slot follows the WAL, so it holds none back');

# killed, it comes back
my $pid = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
ok($node->poll_query_until('postgres',
	qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $pid}),
	'a worker that stops is started again');
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}), 'on its slot');

# with the server, as the slot says, and as the schemas say with no slot
$node->restart;
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}),
	'it starts with the server');

# turned off, the slot goes, and a restart with it on finds the schemas
$node->append_conf('postgresql.conf', 'capture_in_background = off');
$node->reload;
ok($node->poll_query_until('postgres', q{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name LIKE 'prolly_capture_%'}),
	'turned off, the slot goes');
is(workers_of(), '0', 'and the worker');
$node->append_conf('postgresql.conf', 'capture_in_background = on');
$node->restart;
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}),
	'a database with versioned schemas and no slot is found as the server starts');

# a database of its own, and one dropped with its worker running
$node->safe_psql('postgres', 'CREATE DATABASE other');
my $other = $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'other'});
$node->safe_psql('other', 'SELECT 1');
my $on2 = $node->safe_psql('other', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('other', "SET catalog_branch = $on2; CREATE SCHEMA o1 VERSIONED");
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = 'prolly_capture_$other'}),
	'another database has its own worker and slot');
is(workers_of(), '2', 'so two');
$node->safe_psql('postgres', 'DROP DATABASE other');
is(slots_of(), "$slot:true:prolly_capture", 'a database can be dropped with its worker running, and its slot goes');
sleep(6);
is(workers_of(), '1', 'and nothing comes back for it');

# with the last versioned schema it goes
$node->safe_psql('postgres', "$use DROP SCHEMA s1, s2 CASCADE");
ok($node->poll_query_until('postgres', q{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name LIKE 'prolly_capture_%'}),
	'the last versioned schema dropped, the slot goes');
ok($node->poll_query_until('postgres', q{SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'capture worker'}), 'and the worker');
$node->stop;

# with nothing to decode from, nothing happens
my $minimal = PostgreSQL::Test::Cluster->new('minimal');
$minimal->init;
$minimal->append_conf('postgresql.conf', "wal_level = minimal\nmax_wal_senders = 0");
$minimal->start;
my $b = $minimal->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$minimal->safe_psql('postgres', "SET catalog_branch = $b; CREATE SCHEMA m1 VERSIONED");
sleep(2);
is($minimal->safe_psql('postgres', q{SELECT count(*) FROM pg_replication_slots}), '0', 'wal_level minimal has no slot');
is($minimal->safe_psql('postgres', q{SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'capture worker'}), '0', 'nor a worker');
$minimal->stop;

done_testing();
