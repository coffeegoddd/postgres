# Copyright (c) 2026, PostgreSQL Global Development Group

# A table with no key has its rows by the whole row, with how many there are of each.  So does capture,
# and a change always says what it was of, whatever the table's replica identity says.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('keyless');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch;";
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0}) or die 'no worker';
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = current_database()});

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

# rows the same as each other
run('CREATE TABLE t (a integer, b text)');
same_map('t', 'no rows');
run(q{INSERT INTO t VALUES (1, 'x')});
same_map('t', 'a row');
run(q{INSERT INTO t VALUES (1, 'x'), (1, 'x'), (2, NULL), (NULL, NULL)});
same_map('t', 'rows the same, and nulls');
run(q{DELETE FROM t WHERE ctid = (SELECT min(ctid) FROM t WHERE a = 1)});
same_map('t', 'one of those the same deleted');
run(q{UPDATE t SET b = 'y' WHERE ctid = (SELECT min(ctid) FROM t WHERE a = 1)});
same_map('t', 'one of them changed');
run(q{DELETE FROM t WHERE b = 'x'; DELETE FROM t WHERE b = 'y'});
same_map('t', 'the last of each deleted');
run(q{BEGIN; INSERT INTO t VALUES (7, 'z'), (7, 'z'); DELETE FROM t WHERE a = 7; INSERT INTO t VALUES (7, 'z'); UPDATE t SET a = 8 WHERE a = 7; UPDATE t SET a = 7 WHERE a = 8; COMMIT});
same_map('t', 'a row come and gone in a transaction');
run(q{UPDATE t SET b = b});
same_map('t', 'updated to itself');
run(q{DELETE FROM t WHERE a IS NOT DISTINCT FROM 2; INSERT INTO t SELECT g % 4, 'many' FROM generate_series(1, 3000) g});
same_map('t', 'many of few');
run('TRUNCATE t');
same_map('t', 'a truncate');

# nothing but the row itself, and none of it at all
run('CREATE TABLE no_columns (); INSERT INTO no_columns DEFAULT VALUES; INSERT INTO no_columns DEFAULT VALUES');
same_map('no_columns', 'a table with no columns');
run('DELETE FROM no_columns WHERE ctid = (SELECT min(ctid) FROM no_columns)');
same_map('no_columns', 'and one of its rows deleted');

# a value kept out of line: whole in the old row, and not sent again in the new
run(q{CREATE TABLE big (a integer, v text); ALTER TABLE big ALTER v SET STORAGE EXTERNAL; INSERT INTO big VALUES (1, repeat('x', 10000)), (1, repeat('x', 10000)), (2, repeat('y', 10000))});
same_map('big', 'values out of line');
run(q{UPDATE big SET a = 3 WHERE a = 2});
same_map('big', 'left as they were by an update');
run(q{DELETE FROM big WHERE ctid = (SELECT min(ctid) FROM big WHERE a = 1)});
same_map('big', 'and one deleted');

# what the table says of its replica identity isn't what's logged
for my $case (
	[ 'nothing', 'id integer PRIMARY KEY, v text', 'REPLICA IDENTITY NOTHING' ],
	[ 'deferred_key', 'id integer PRIMARY KEY DEFERRABLE, v text', undef ],
	[ 'by_index', 'id integer PRIMARY KEY, v text NOT NULL UNIQUE', 'REPLICA IDENTITY USING INDEX by_index_v_key' ],
	[ 'full_key', 'id integer PRIMARY KEY, v text', 'REPLICA IDENTITY FULL' ],
	[ 'no_key_nothing', 'id integer, v text', 'REPLICA IDENTITY NOTHING' ])
{
	my ($name, $columns, $alter) = @$case;
	run("CREATE TABLE $name ($columns)");
	run("ALTER TABLE $name $alter") if $alter;
	run("INSERT INTO $name VALUES (1, 'a'), (2, 'b')");
	same_map($name, "$name: inserts");
	run("UPDATE $name SET id = 9 WHERE id = 1");
	same_map($name, "$name: a key changed");
	run("UPDATE $name SET v = 'c' WHERE id = 9");
	same_map($name, "$name: a value changed");
	run("DELETE FROM $name WHERE id = 2");
	same_map($name, "$name: a delete");
}
is(run(q{SELECT relreplident FROM pg_class WHERE oid = 's.nothing'::regclass}), 'n', 'and what it says is left alone');

# changing, row by row
run(q{CREATE TABLE changing (id integer PRIMARY KEY, v text); INSERT INTO changing VALUES (1, 'a'), (2, 'a')});
run('ALTER TABLE changing REPLICA IDENTITY NOTHING; UPDATE changing SET id = 9 WHERE id = 1');
same_map('changing', 'replica identity changed after a table was seen');
run('ALTER TABLE changing REPLICA IDENTITY DEFAULT; ALTER TABLE changing DROP CONSTRAINT changing_pkey');
same_map('changing', 'a key dropped');
run(q{INSERT INTO changing VALUES (9, 'a'); DELETE FROM changing WHERE id = 2});
same_map('changing', 'and written');
run(q{DELETE FROM changing WHERE ctid = (SELECT min(ctid) FROM changing); ALTER TABLE changing ADD PRIMARY KEY (id)});
same_map('changing', 'a key added');
run(q{UPDATE changing SET id = 10; INSERT INTO changing VALUES (11, 'b')});
same_map('changing', 'and written');
run(q{BEGIN; ALTER TABLE changing DROP CONSTRAINT changing_pkey; INSERT INTO changing VALUES (11, 'b'); DELETE FROM changing WHERE id = 10; COMMIT});
same_map('changing', 'a key dropped, and written, in a transaction');
run(q{ALTER TABLE changing ADD COLUMN extra integer DEFAULT 5});
same_map('changing', 'rows told whole');
run(q{INSERT INTO changing VALUES (11, 'b', 5); UPDATE changing SET extra = 6 WHERE ctid = (SELECT min(ctid) FROM changing)});
same_map('changing', 'and written after');

# a partition
run(q{CREATE TABLE parted (a integer, b text) PARTITION BY RANGE (a); CREATE TABLE parted1 PARTITION OF parted FOR VALUES FROM (0) TO (10); CREATE TABLE parted2 PARTITION OF parted FOR VALUES FROM (10) TO (20);
	INSERT INTO parted VALUES (1, 'x'), (1, 'x'), (11, 'y'); UPDATE parted SET a = 12 WHERE ctid = (SELECT min(ctid) FROM parted1) AND a = 1});
same_map('parted1', 'a row moved out of a partition');
same_map('parted2', 'and into another');

# a new slot reads them, across its batches
run(q{CREATE TABLE across (a integer); INSERT INTO across SELECT 1 FROM generate_series(1, 2500); INSERT INTO across SELECT g FROM generate_series(2, 1200) g});
caught_up();
my $mark = $node->safe_psql('postgres', 'SELECT lsn FROM pg_history.capture_positions WHERE branch = 0');
$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()');
$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'slot stayed';
$node->safe_psql('postgres', 'ALTER SYSTEM SET capture_in_background = on; SELECT pg_reload_conf()');
$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0 AND lsn > '$mark'}) or die 'no new slot';
same_map('across', 'a new slot, with one row many times over');
same_map('t', 'and a table it was following');
run(q{DELETE FROM across WHERE ctid = (SELECT min(ctid) FROM across WHERE a = 1)});
same_map('across', 'which goes on');

# a count isn't taken twice, as a key's value could be: applied, not confirmed, and told again
SKIP:
{
	skip 'injection points not supported by this build', 3 if $ENV{enable_injection_points} ne 'yes';
	my $other = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
	my $on_other = "SET catalog_branch = $other;";
	$node->safe_psql('postgres', "$on_other CREATE SCHEMA s VERSIONED; CREATE TABLE s.again (a integer)");
	run('CREATE TABLE again (a integer); INSERT INTO again VALUES (1)');
	caught_up();
	$node->safe_psql('postgres', "CREATE EXTENSION injection_points; SELECT injection_points_attach('capture-applied', 'wait')");
	$node->safe_psql('postgres',
		    "BEGIN; SET LOCAL catalog_branch = $branch; INSERT INTO s.again VALUES (1), (1), (2);"
		  . "SET LOCAL catalog_branch = $other; INSERT INTO s.again VALUES (1), (3); COMMIT");
	$node->wait_for_event('capture worker', 'capture-applied');
	my $held = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});

	# (with what's not been applied after it, so that one read has both)
	run('INSERT INTO again VALUES (1), (4)');
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applied')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($held)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $held})
	  or die 'no worker came back';
	same_map('again', 'a transaction applied and told again');
	my $was = $on;
	$on = $on_other;
	same_map('again', 'on each of its branches');
	$on = $was;
	run('DELETE FROM again WHERE ctid = (SELECT min(ctid) FROM again WHERE a = 1)');
	same_map('again', 'and removing one goes on from the right count');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
}

# a map out of step with its table (as a value naming an object, now refused, once made one): an
# error naming the table, which a rewrite gets past
run(q{CREATE TABLE names (r text, n integer); INSERT INTO names VALUES ('s.named', 1); CREATE TABLE no_names (r text, n integer)});
caught_up();
$node->safe_psql('postgres', qq{$on UPDATE pg_history.pointers SET hash = (SELECT hash FROM pg_history.pointers WHERE name = 'capture/map/' || 's.no_names'::regclass::oid)
	WHERE name = 'capture/map/' || 's.names'::regclass::oid});
my $log_at = -s $node->logfile;
run('DELETE FROM names');
ok($node->wait_for_log(qr/fewer of a keyless row than were removed/, $log_at), 'a row the map does not have, removed');
like(slurp_file($node->logfile, $log_at), qr/capture applying changes to table names of relation \d+/, 'says which table');

# a barrier meanwhile gives up, and its marker doesn't stand in the way of what gets past it
my ($rcb, $outb, $errb) = $node->psql('postgres', "$on SELECT test_capture_barrier('s', 200)");
like($errb, qr/capture did not reach/, 'a barrier while capture has stopped');
run('VACUUM FULL names');
same_map('names', 'and a rewrite gets past it');
same_map('t', 'and capture goes on');

# only what capture holds logs more, and a shared table's replica identity means what it says
sub delete_wal
{
	my ($table, $on_branch) = @_;
	my $set = $on_branch ? $on : '';
	$node->safe_psql('postgres', qq{$set INSERT INTO $table VALUES (1, (SELECT string_agg(md5(g::text), '') FROM generate_series(1, 60) g))});
	caught_up();
	my $at = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$node->safe_psql('postgres', "$set DELETE FROM $table");
	return $node->safe_psql('postgres', qq{SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '$at')});
}
$node->safe_psql('postgres', 'CREATE SCHEMA shared; CREATE TABLE shared.u (a integer, v text); ALTER TABLE shared.u ALTER v SET STORAGE PLAIN');
run('CREATE TABLE sized (a integer, v text); ALTER TABLE sized ALTER v SET STORAGE PLAIN');
cmp_ok(delete_wal('shared.u', 0), '<', 1000, 'a shared table with no key logs no old row');
cmp_ok(delete_wal('s.sized', 1), '>', 1000, 'a versioned one does');

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal/m, 'nothing crashed');
$node->stop;
done_testing();
