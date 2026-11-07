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
is(run(q{SELECT pg_column_size(w) < 1300 AND pg_column_size(v) > 5000 FROM t WHERE id = 1}), 't', 'of both lengths');
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

# a value left as it was stays in the tree it's in: nothing's written for it
run(q{CREATE TABLE same_tree (id integer PRIMARY KEY, n integer, v text); ALTER TABLE same_tree ALTER v SET STORAGE EXTERNAL;
	INSERT INTO same_tree VALUES (1, 0, (SELECT string_agg(md5(g::text), '') FROM generate_series(1, 200000) g))});
caught_up();
my $chunks = q{SELECT count(*) FROM pg_history.chunks};
my $before = $node->safe_psql('postgres', $chunks);
run(q{UPDATE same_tree SET n = 1});
same_map('same_tree', 'a value of 6 MB left as it was');
cmp_ok($node->safe_psql('postgres', $chunks) - $before, '<', 20, 'writes no tree for it');

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal/m, 'nothing crashed');
$node->stop;
done_testing();

# (a failure with the worker stopped would leave the server unable to stop)
END
{
	kill 'CONT', $worker if $worker && !$windows_os;
}
