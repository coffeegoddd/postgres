# Copyright (c) 2026, PostgreSQL Global Development Group

# An update doesn't send a value it left out of line.  Capture has what the row has now: from what's
# being applied with it, or else from the row map, and the map comes out as reading the table makes it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('toast');
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

# from the map, in a tree (long) and in a cell (short, kept out of line all the same)
run(q{CREATE TABLE t (id integer PRIMARY KEY, n integer, v text, w text, x text);
	ALTER TABLE t ALTER v SET STORAGE EXTERNAL, ALTER w SET STORAGE EXTERNAL, ALTER x SET STORAGE EXTERNAL;
	INSERT INTO t SELECT g, 0, repeat('v' || g, 5000), repeat('w', 1200), repeat('x', 1200) FROM generate_series(1, 5) g});
same_map('t', 'values out of line');
is(run(q{SELECT pg_column_toast_chunk_id(w) IS NOT NULL AND pg_column_size(w) < 1300 AND pg_column_toast_chunk_id(v) IS NOT NULL AND pg_column_size(v) > 5000 FROM t WHERE id = 1}), 't', 'of both lengths');
run('UPDATE t SET n = 1');
same_map('t', 'an update leaving them');
run(q{UPDATE t SET n = 2 WHERE id = 1; UPDATE t SET n = 3 WHERE id = 1});
same_map('t', 'and again');
run(q{UPDATE t SET w = 'short now' WHERE id = 2});
same_map('t', 'an update changing one and leaving another');
run(q{UPDATE t SET v = NULL WHERE id = 3; UPDATE t SET n = 4 WHERE id = 3});
same_map('t', 'one of them null');

# from what's being applied with it, in a transaction and in several before the worker's told of them
run(q{BEGIN; UPDATE t SET v = repeat('changed', 5000) WHERE id = 1; UPDATE t SET n = 5 WHERE id = 1; COMMIT});
same_map('t', 'changed, then left, in a transaction');
run(q{BEGIN; INSERT INTO t VALUES (100, 0, repeat('new', 5000), repeat('w', 1200), 'x'); UPDATE t SET n = 6 WHERE id = 100; COMMIT});
same_map('t', 'inserted, then left');
run(q{BEGIN; UPDATE t SET n = 7 WHERE id = 100; DELETE FROM t WHERE id = 100; INSERT INTO t VALUES (100, 0, repeat('again', 5000), 'w', 'x'); UPDATE t SET n = 8 WHERE id = 100; COMMIT});
same_map('t', 'left, deleted, inserted anew, and left');
run(q{BEGIN; SAVEPOINT a; UPDATE t SET v = 'rolled back' WHERE id = 2; ROLLBACK TO a; UPDATE t SET n = 9 WHERE id = 2; COMMIT});
same_map('t', 'a change rolled back, then left');
SKIP:
{
	skip 'no way to stop a process', 1 if $windows_os;
	caught_up();
	kill 'STOP', $worker;
	run(q{UPDATE t SET v = repeat('first', 5000) WHERE id = 4});
	run(q{UPDATE t SET n = 10 WHERE id = 4});
	run(q{UPDATE t SET v = repeat('second', 5000) WHERE id = 4});
	run(q{UPDATE t SET n = 11 WHERE id = 4});
	kill 'CONT', $worker;
	same_map('t', 'several transactions applied at once');
}

# the key changing as well: found by the key it had
run(q{UPDATE t SET id = 200 WHERE id = 2});
same_map('t', 'a key changed, leaving them');
run(q{BEGIN; UPDATE t SET id = 201 WHERE id = 200; UPDATE t SET id = 202 WHERE id = 201; UPDATE t SET n = 12 WHERE id = 202; COMMIT});
same_map('t', 'changed over and over');
run(q{BEGIN; UPDATE t SET id = 300 WHERE id = 3; INSERT INTO t VALUES (3, 0, repeat('took its place', 5000), 'w', 'x'); UPDATE t SET n = 13 WHERE id = 3; UPDATE t SET n = 14 WHERE id = 300; COMMIT});
same_map('t', 'and another row at the key it had');
run(q{DELETE FROM t WHERE id = 202});
same_map('t', 'and one deleted after');

# a key kept out of line
run(q{CREATE TABLE by_big (k text PRIMARY KEY, n integer, v text); ALTER TABLE by_big ALTER k SET STORAGE EXTERNAL, ALTER v SET STORAGE EXTERNAL;
	INSERT INTO by_big VALUES (repeat('k', 2600), 0, repeat('v', 9000))});
same_map('by_big', 'a key out of line');
run(q{UPDATE by_big SET n = 1});
same_map('by_big', 'an update leaving it, and a value');

# other replica identities: the old row has only what a key does, or everything
run(q{CREATE TABLE full_id (id integer PRIMARY KEY, n integer, v text); ALTER TABLE full_id ALTER v SET STORAGE EXTERNAL; ALTER TABLE full_id REPLICA IDENTITY FULL;
	INSERT INTO full_id VALUES (1, 0, repeat('v', 9000)); UPDATE full_id SET n = 1});
same_map('full_id', 'replica identity full');
run(q{CREATE TABLE no_id (id integer PRIMARY KEY, n integer, v text); ALTER TABLE no_id ALTER v SET STORAGE EXTERNAL; ALTER TABLE no_id REPLICA IDENTITY NOTHING;
	INSERT INTO no_id VALUES (1, 0, repeat('v', 9000)); UPDATE no_id SET n = 1});
same_map('no_id', 'replica identity nothing');

# rows told whole, and after a new slot: from a table's map as it was begun
run(q{ALTER TABLE t ADD COLUMN extra integer DEFAULT 5});
same_map('t', 'told whole');
run(q{UPDATE t SET n = 15 WHERE id = 1});
same_map('t', 'and left after');
run(q{BEGIN; ALTER TABLE t ADD COLUMN extra2 integer DEFAULT 6; UPDATE t SET n = 16 WHERE id = 1; COMMIT});
same_map('t', 'told whole and left in one transaction');
my $mark = $node->safe_psql('postgres', 'SELECT lsn FROM pg_history.capture_positions WHERE branch = 0');
$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
run(q{UPDATE t SET n = 17 WHERE id = 1});
$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0 AND lsn > '$mark'}) or die 'no new slot';
same_map('t', 'a new slot');
run(q{UPDATE t SET n = 18 WHERE id = 1});
same_map('t', 'and left after it');

# other ways an update comes about, and a value that looks left but isn't
run(q{CREATE TABLE ways (id integer PRIMARY KEY, n integer, v text, w text); ALTER TABLE ways ALTER v SET STORAGE EXTERNAL, ALTER w SET STORAGE EXTERNAL;
	INSERT INTO ways SELECT g, 0, repeat('v' || g, 5000), repeat('w' || g, 5000) FROM generate_series(1, 6) g});
caught_up();
run(q{INSERT INTO ways VALUES (1, 0, 'ignored', 'ignored') ON CONFLICT (id) DO UPDATE SET n = 1});
same_map('ways', 'ON CONFLICT');
run(q{MERGE INTO ways USING (VALUES (2)) x(i) ON id = i WHEN MATCHED THEN UPDATE SET n = 2; MERGE INTO ways USING (VALUES (3)) x(i) ON id = i WHEN MATCHED THEN UPDATE SET id = 30});
same_map('ways', 'MERGE');
run(q{UPDATE ways SET w = v WHERE id = 4; UPDATE ways SET v = w, w = v WHERE id = 5});
same_map('ways', 'a column copied, and two swapped');
run(q{UPDATE ways a SET v = b.v FROM ways b WHERE a.id = 6 AND b.id = 1});
same_map('ways', 'a value from another row');
run(q{CREATE TABLE moved (id integer PRIMARY KEY, n integer, v text) PARTITION BY RANGE (id); CREATE TABLE moved1 PARTITION OF moved FOR VALUES FROM (0) TO (10); CREATE TABLE moved2 PARTITION OF moved FOR VALUES FROM (10) TO (20);
	ALTER TABLE moved ALTER v SET STORAGE EXTERNAL; INSERT INTO moved VALUES (1, 0, repeat('m', 9000)); UPDATE moved SET id = 11 WHERE id = 1; UPDATE moved SET n = 1 WHERE id = 11});
same_map('moved2', 'a row moved between partitions, and left');

# from rows told whole in the same apply, and told again over a map already past it
SKIP:
{
	skip 'no way to stop a process', 2 if $windows_os || $ENV{enable_injection_points} ne 'yes';
	caught_up();
	kill 'STOP', $worker;
	run(q{ALTER TABLE ways ADD COLUMN extra integer DEFAULT 1});
	run(q{UPDATE ways SET n = 10 WHERE id = 1});
	kill 'CONT', $worker;
	same_map('ways', 'told whole, then left, applied together');
	caught_up();
	kill 'STOP', $worker;
	run(q{UPDATE ways SET n = 11 WHERE id = 6});
	run(q{DELETE FROM ways WHERE id = 6});
	$node->safe_psql('postgres', "CREATE EXTENSION injection_points; SELECT injection_points_attach('capture-applied', 'wait')");
	kill 'CONT', $worker;
	$node->wait_for_event('capture worker', 'capture-applied');
	my $held = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applied')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($held)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $held})
	  or die 'no worker came back';
	$worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	same_map('ways', 'told again, over a map past it');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
}

# a deferrable key can be on two rows for a while
run(q{CREATE TABLE d (id integer PRIMARY KEY DEFERRABLE, n integer, v text); INSERT INTO d SELECT g, g, 'v' || g FROM generate_series(1, 4) g});
same_map('d', 'a deferrable key');
run(q{UPDATE d SET id = id + 1});
same_map('d', 'keys moved onto each other');
run(q{BEGIN; SET CONSTRAINTS ALL DEFERRED; UPDATE d SET id = 100 WHERE id = 2; UPDATE d SET id = 2 WHERE id = 3; UPDATE d SET id = 3 WHERE id = 100; COMMIT});
same_map('d', 'two swapped');
run(q{BEGIN; SET CONSTRAINTS ALL DEFERRED; INSERT INTO d VALUES (50, 0, 'a'); DELETE FROM d WHERE id = 50; INSERT INTO d VALUES (51, 0, 'b'); UPDATE d SET n = 1 WHERE id = 51; DELETE FROM d WHERE id = 51; COMMIT});
same_map('d', 'rows come and gone');
run(q{BEGIN; SET CONSTRAINTS ALL DEFERRED; INSERT INTO d VALUES (60, 1, 'same'), (61, 1, 'same'); COMMIT; BEGIN; SET CONSTRAINTS ALL DEFERRED; UPDATE d SET id = 61 WHERE id = 60; DELETE FROM d WHERE id = 61 AND ctid = (SELECT max(ctid) FROM d WHERE id = 61); COMMIT});
same_map('d', 'a row moved onto one with the same value, and one of them deleted');
run(q{ALTER TABLE d ALTER v SET STORAGE EXTERNAL; UPDATE d SET v = repeat(id::text, 9000)});
same_map('d', 'out of line');
run(q{UPDATE d SET id = id + 1; UPDATE d SET n = n + 1});
same_map('d', 'moved onto each other and left, the old row having them');
run(q{CREATE TABLE d2 (id integer PRIMARY KEY DEFERRABLE, n integer); INSERT INTO d2 VALUES (1, 1)});
same_map('d2', 'before its key is made immediate');
my $log_at = -s $node->logfile;
$node->safe_psql('postgres', "$on BEGIN; UPDATE s.d2 SET n = 2; ALTER TABLE s.d2 DROP CONSTRAINT d2_pkey; ALTER TABLE s.d2 ADD PRIMARY KEY (id); UPDATE s.d2 SET n = 3; COMMIT");
ok($node->wait_for_log(qr/does not follow relation \d+ having its deferrable key changed/, $log_at), 'made immediate in a transaction that writes it: an error');
run('VACUUM FULL d2');
same_map('d2', 'which a rewrite gets past');

# several of a table's transactions in one apply, its key deferrable for some of them
SKIP:
{
	skip 'no way to stop a process', 3 if $windows_os;
	run(q{CREATE TABLE e (id integer PRIMARY KEY, n integer, v text); ALTER TABLE e ALTER v SET STORAGE EXTERNAL;
		INSERT INTO e SELECT g, 0, repeat(g::text, 9000) FROM generate_series(1, 4) g});
	caught_up();
	kill 'STOP', $worker;
	run(q{UPDATE e SET v = repeat('first', 5000) WHERE id = 3});
	run(q{INSERT INTO e VALUES (50, 0, 'gone')});
	run(q{DELETE FROM e WHERE id = 50});

	# (in a transaction of their own, or the rows would be told whole, leaving nothing to get wrong)
	run(q{BEGIN; ALTER TABLE e DROP CONSTRAINT e_pkey; ALTER TABLE e ADD PRIMARY KEY (id) DEFERRABLE; COMMIT});
	run(q{UPDATE e SET id = id + 1});
	run(q{UPDATE e SET id = id + 1});
	run(q{BEGIN; ALTER TABLE e DROP CONSTRAINT e_pkey; ALTER TABLE e ADD PRIMARY KEY (id); COMMIT});
	run(q{UPDATE e SET n = 1 WHERE id = 3});
	run(q{UPDATE e SET v = repeat('second', 5000) WHERE id = 4});
	run(q{UPDATE e SET n = 2 WHERE id = 4});
	kill 'CONT', $worker;
	same_map('e', 'immediate, deferrable twice, and immediate, in one apply');

	kill 'STOP', $worker;
	run(q{VACUUM FULL d});
	run(q{UPDATE d SET id = id + 1});
	kill 'CONT', $worker;
	same_map('d', 'told whole, then deferrable, in one apply');

	my $from = -s $node->logfile;
	$node->safe_psql('postgres', "$on BEGIN; UPDATE s.e SET n = 3; ALTER TABLE s.e DROP CONSTRAINT e_pkey; ALTER TABLE s.e ADD PRIMARY KEY (id) DEFERRABLE; UPDATE s.e SET n = 4; COMMIT");
	ok($node->wait_for_log(qr/does not follow relation \d+ having its deferrable key changed/, $from), 'made deferrable in a transaction that writes it: an error');
	run('VACUUM FULL e');
	same_map('e', 'which a rewrite gets past');
}

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal/m, 'nothing crashed');
$node->stop;
done_testing();

# (a failure with the worker stopped would leave the server unable to stop)
END
{
	kill 'CONT', $worker if $worker && !$windows_os;
}
