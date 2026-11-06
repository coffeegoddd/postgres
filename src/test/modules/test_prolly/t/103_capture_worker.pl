# Copyright (c) 2026, PostgreSQL Global Development Group

# One capture worker per database with a versioned schema, on one logical slot: started with its
# first versioned schema and with the server, gone with the last, and in nothing's way: a
# database with none is never connected to.

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

# with the server
$node->restart;
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}),
	'it starts with the server');

# turned off, the slot goes, and on again it is back, with no restart
$node->append_conf('postgresql.conf', 'capture_in_background = off');
$node->reload;
ok($node->poll_query_until('postgres', q{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name LIKE 'prolly_capture_%'}),
	'turned off, the slot goes');
is(workers_of(), '0', 'and the worker');
$node->append_conf('postgresql.conf', 'capture_in_background = on');
$node->reload;
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = '$slot'}),
	'turned on, a database with versioned schemas has its worker again');

# a database that cannot be dropped keeps its worker
{
	my $held = $node->background_psql('postgres');
	$node->safe_psql('postgres', 'CREATE DATABASE busy');
	my $busy = $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'busy'});
	my $b_on = $node->safe_psql('busy', 'SELECT pg_branch_id_allocate()');
	$node->safe_psql('busy', "SET catalog_branch = $b_on; CREATE SCHEMA o1 VERSIONED");
	ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = 'prolly_capture_$busy'}), 'a second database has its worker');
	my $user = $node->background_psql('busy');
	$user->query_safe('SELECT 1');
	my ($rc, $out, $err) = $node->psql('postgres', 'DROP DATABASE busy');
	like($err, qr/is being accessed by other users/, 'a drop refused');
	ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = 'prolly_capture_$busy'}),
		'and the worker is back on its slot, which is still there');
	$user->quit;

	# copied from one, a database has what it was copied from
	$node->safe_psql('busy', 'SELECT 1');
	my $src = $node->safe_psql('postgres', q{SELECT datversioned FROM pg_database WHERE datname = 'busy'});
	is($src, 't', 'a database with a versioned schema is marked');
	$node->safe_psql('postgres', 'DROP DATABASE busy');
	$held->quit;
	is($node->safe_psql('postgres', qq{SELECT count(*) FROM pg_replication_slots WHERE slot_name = 'prolly_capture_$busy'}), '0', 'dropped, its slot goes');
}

$node->safe_psql('postgres', 'CREATE DATABASE plain_one');
is($node->safe_psql('postgres', q{SELECT datversioned FROM pg_database WHERE datname = 'plain_one'}), 'f', 'a database with none is not marked');
sleep(6);
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_stat_activity WHERE datname = 'plain_one'}), '0', 'and nothing connects to it');

$node->safe_psql('postgres', 'CREATE DATABASE tmpl');
my $tmpl_on = $node->safe_psql('tmpl', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('tmpl', "SET catalog_branch = $tmpl_on; CREATE SCHEMA ts VERSIONED");
$node->safe_psql('postgres', 'CREATE DATABASE clone TEMPLATE tmpl');
my $clone = $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'clone'});
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = 'prolly_capture_$clone'}),
	'a database copied from one with versioned schemas has its worker, with no restart');

$node->safe_psql('postgres', q{DROP DATABASE clone});
$node->safe_psql('postgres', q{DROP DATABASE tmpl});

# a slot of its name that isn't capture's is left alone, and said so
$node->safe_psql('postgres', 'CREATE DATABASE named');
my $named = $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'named'});
$node->safe_psql('postgres', qq{SELECT pg_create_physical_replication_slot('prolly_capture_$named')});
my $named_on = $node->safe_psql('named', 'SELECT pg_branch_id_allocate()');
my $at = -s $node->logfile;
$node->safe_psql('named', "SET catalog_branch = $named_on; CREATE SCHEMA ns VERSIONED");
ok($node->wait_for_log(qr/replication slot "prolly_capture_$named" is not capture's/, $at), 'a slot of its name that is not capture\'s is said');
is($node->safe_psql('postgres', qq{SELECT slot_type FROM pg_replication_slots WHERE slot_name = 'prolly_capture_$named'}), 'physical', 'and left alone');
$node->safe_psql('postgres', qq{SELECT pg_drop_replication_slot('prolly_capture_$named')});
$node->safe_psql('postgres', 'DROP DATABASE named');

# a database that takes no connections is captured all the same
$node->safe_psql('postgres', 'CREATE DATABASE closed');
my $closed = $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'closed'});
my $closed_on = $node->safe_psql('closed', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('closed', "SET catalog_branch = $closed_on; CREATE SCHEMA cs VERSIONED");
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = 'prolly_capture_$closed'}), 'it has its worker');
$node->safe_psql('postgres', 'ALTER DATABASE closed ALLOW_CONNECTIONS false');
my $closed_pid = $node->safe_psql('postgres', qq{SELECT active_pid FROM pg_replication_slots WHERE slot_name = 'prolly_capture_$closed'});
$node->safe_psql('postgres', "SELECT pg_terminate_backend($closed_pid)");
ok($node->poll_query_until('postgres', qq{SELECT active_pid IS NOT NULL AND active_pid <> $closed_pid FROM pg_replication_slots WHERE slot_name = 'prolly_capture_$closed'}),
	'with connections refused, a worker still comes back');
$node->safe_psql('postgres', 'ALTER DATABASE closed ALLOW_CONNECTIONS true');
$node->safe_psql('postgres', 'DROP DATABASE closed');

# as a prepared transaction commits
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 5');
$node->restart;
$node->safe_psql('postgres', 'CREATE DATABASE prep');
my $prep = $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'prep'});
my $prep_on = $node->safe_psql('prep', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('prep', "SET catalog_branch = $prep_on; BEGIN; CREATE SCHEMA ps VERSIONED; PREPARE TRANSACTION 'p1'");
$node->safe_psql('prep', "SET catalog_branch = $prep_on; COMMIT PREPARED 'p1'");
ok($node->poll_query_until('postgres', qq{SELECT active FROM pg_replication_slots WHERE slot_name = 'prolly_capture_$prep'}), 'a prepared first schema has its worker once it commits');
$node->safe_psql('postgres', 'DROP DATABASE prep');

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
ok($node->poll_query_until('postgres', q{SELECT NOT datversioned FROM pg_database WHERE datname = 'postgres'}), 'the last versioned schema dropped, the mark clears');
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
