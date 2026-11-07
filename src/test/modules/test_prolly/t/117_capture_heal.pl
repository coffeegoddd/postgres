# Copyright (c) 2026, PostgreSQL Global Development Group

# A table whose changes capture can't apply is healed: its rows told whole again, and the rest
# going on meanwhile, rather than capture stopping for the whole database.  A commit from capture of
# its schema is refused until it's healed.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('heal');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch;";
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0}) or die 'no worker';
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = current_database()});
my $worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});

sub run
{
	return $node->safe_psql('postgres', "$on SET search_path = s, public; $_[0]");
}

sub caught_up
{
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die 'capture did not catch up';
}

sub same_map
{
	my ($table, $what) = @_;
	caught_up();
	is(run(qq{SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.$table'::regclass::oid}),
		run(qq{SELECT test_ws_rows_hash('s', '$table')}), $what);
}

sub healed
{
	my ($table) = @_;

	# (the query alone, with no SET before it, which would print a line of its own)
	my $relid = run(qq{SELECT 's.$table'::regclass::oid});
	$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_history.capture_healing WHERE relid = $relid})
	  or die "$table was not healed";
}

# Has a keyless table's map out of step with it, as a change capture can't follow would.
sub out_of_step
{
	my ($table) = @_;
	run(qq{CREATE TABLE IF NOT EXISTS empty_one (a integer, v text)});
	caught_up();
	$node->safe_psql('postgres', qq{$on UPDATE pg_history.pointers SET hash = (SELECT hash FROM pg_history.pointers WHERE name = 'capture/map/' || 's.empty_one'::regclass::oid)
		WHERE name = 'capture/map/' || 's.$table'::regclass::oid});
}

run(q{CREATE TABLE bad (a integer, v text); INSERT INTO bad VALUES (1, 'one'), (2, 'two');
	CREATE TABLE good (id integer PRIMARY KEY, v text); INSERT INTO good VALUES (1, 'one')});
same_map('bad', 'a table');
same_map('good', 'and another');

# a change that can't be applied: the table's healed, and the rest goes on
out_of_step('bad');
my $log_at = -s $node->logfile;
run(q{DELETE FROM bad WHERE a = 1; INSERT INTO good VALUES (2, 'two')});
ok($node->wait_for_log(qr/WARNING:  capture can't apply changes of relation \d+: a row map holds fewer of a keyless row/, $log_at), 'what was wrong is logged');
healed('bad');
same_map('bad', 'healed: its map is its rows');
same_map('good', 'and the other table went on');
is($node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'}), $worker, 'with the worker as it was');
run(q{INSERT INTO bad VALUES (3, 'three'); DELETE FROM bad WHERE a = 2});
same_map('bad', 'and written after');

# while a writer has it, it's left to heal later, and capture and commits go on meanwhile
{
	out_of_step('bad');
	my $writer = $node->background_psql('postgres');
	$writer->query_safe("$on BEGIN; INSERT INTO s.bad VALUES (4, 'open')");
	$log_at = -s $node->logfile;
	run(q{DELETE FROM bad WHERE a = 3});
	ok($node->wait_for_log(qr/capture can't apply changes of relation/, $log_at), 'a change that cannot be applied, with a writer open');
	is(run(q{SELECT count(*) FROM pg_history.capture_healing WHERE relid = 's.bad'::regclass}), '1', 'it is being healed');
	run(q{INSERT INTO good VALUES (3, 'three')});
	same_map('good', 'and other tables go on meanwhile');
	my ($rc, $out, $err) = $node->psql('postgres', "$on SELECT test_capture_barrier('s', 30000)");
	like($err, qr/capture is healing table "bad" of schema "s"/, 'a commit of its schema is refused meanwhile');
	$writer->query_safe('COMMIT');
	$writer->quit;
	healed('bad');
	same_map('bad', 'and once the writer is done, it is healed');
	isnt(run(q{SELECT test_capture_barrier('s')}), '', 'and a commit can be made');
}

# what can't be followed (a key made deferrable, with writes on both sides) heals with no rewrite
run(q{CREATE TABLE keyed (id integer PRIMARY KEY, n integer); INSERT INTO keyed VALUES (1, 1)});
caught_up();
$log_at = -s $node->logfile;
$node->safe_psql('postgres', "$on BEGIN; UPDATE s.keyed SET n = 2; ALTER TABLE s.keyed DROP CONSTRAINT keyed_pkey; ALTER TABLE s.keyed ADD PRIMARY KEY (id) DEFERRABLE; UPDATE s.keyed SET n = 3; COMMIT");
ok($node->wait_for_log(qr/capture can't apply changes of relation \d+: capture to trees does not follow relation \d+ having its deferrable key changed/, $log_at), 'what capture does not follow');
healed('keyed');
same_map('keyed', 'is healed, with nothing done to the table');

# a crash before it's told whole: told whole when the worker starts again
SKIP:
{
	skip 'injection points not supported by this build', 2 if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', "CREATE EXTENSION injection_points");
	out_of_step('bad');
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-applied', 'wait')});
	run(q{DELETE FROM bad WHERE a = 4});
	$node->wait_for_event('capture worker', 'capture-applied');
	is(run(q{SELECT count(*) FROM pg_history.capture_healing WHERE relid = 's.bad'::regclass}), '1', 'recorded to heal, not told yet');
	my $held = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applied')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($held)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $held})
	  or die 'no worker came back';
	healed('bad');
	same_map('bad', 'healed by the worker that came after');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
}

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal/m, 'nothing crashed');
$node->stop;
done_testing();
