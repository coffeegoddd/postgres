# Copyright (c) 2026, PostgreSQL Global Development Group

# DDL that changes a table's rows and tells no change of them: the statement writes the rows into
# the WAL, and a table's row map is still, to the byte, the one layer 4 makes by reading it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('ddl');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf',
	"log_min_messages = debug1\nlog_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\nlogical_decoding_work_mem = 64kB\nmax_prepared_transactions = 2\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $b1 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b2 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on1 = "SET catalog_branch = $b1;";
my $on2 = "SET catalog_branch = $b2;";
$node->safe_psql('postgres', "$on1 CREATE SCHEMA s VERSIONED");
$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0}) or die 'no worker';
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});

sub caught_up
{
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres',
		qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die "capture did not reach $to";
}

sub same_map
{
	my ($table, $what, $on) = @_;
	$on //= $on1;
	caught_up();
	is( $node->safe_psql('postgres',
			"$on SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.$table'::regclass::oid"),
		$node->safe_psql('postgres', "$on SELECT test_ws_rows_hash('s', '$table')"),
		$what);
}

sub run
{
	my ($sql, $on) = @_;
	$on //= $on1;
	$node->safe_psql('postgres', "$on SET search_path = s, public; $sql");
}

# A statement, then the rows it changed, with a row written after it too.
run(q{CREATE TABLE t (id integer PRIMARY KEY, a integer, v text); INSERT INTO t SELECT g, g * 10, 'v' || g FROM generate_series(1, 3000) g});
same_map('t', 'a table with rows');
run('ALTER TABLE t ADD COLUMN plain integer');
same_map('t', 'a column added, with no default');
run(q{ALTER TABLE t ADD COLUMN d text DEFAULT 'dflt'});
same_map('t', 'a column added with a default, in no row\'s bytes');
run(q{ALTER TABLE t ADD COLUMN r float8 DEFAULT random()});
same_map('t', 'a column added with a default computed for each row');
run('ALTER TABLE t DROP COLUMN plain');
same_map('t', 'a column dropped, all of it NULL, which no row\'s bytes hold');
run(q{ALTER TABLE t ADD COLUMN held text DEFAULT 'held'; ALTER TABLE t DROP COLUMN held});
same_map('t', 'a column dropped, with values');
run('ALTER TABLE t ALTER COLUMN a TYPE bigint');
same_map('t', 'a column retyped');
run('ALTER TABLE t ALTER COLUMN a TYPE bigint USING a + 1');
same_map('t', 'a retype to the same type, with new values');
run(q{ALTER TABLE t ALTER COLUMN v TYPE text USING upper(v)});
same_map('t', 'and a text one');
run('ALTER TABLE t DROP COLUMN d, ADD COLUMN d text');
same_map('t', 'a column dropped and added in one statement');
run('ALTER TABLE t DROP COLUMN d; ALTER TABLE t ADD COLUMN d text');
same_map('t', 'and in two');
run(q{ALTER TABLE t ADD COLUMN g integer GENERATED ALWAYS AS (a + 1) STORED});
same_map('t', 'a stored generated column added');
run(q{ALTER TABLE t ALTER COLUMN g SET EXPRESSION AS (a + 2)});
same_map('t', 'its expression changed');
run('ALTER TABLE t RENAME COLUMN v TO w');
same_map('t', 'a column renamed');
run('ALTER TABLE t DROP CONSTRAINT t_pkey, ADD PRIMARY KEY (a)');
same_map('t', 'another primary key, which every row is keyed by');
run(q{INSERT INTO t (id, a, w) VALUES (9001, -1, 'after'); UPDATE t SET w = 'changed' WHERE id = 1; DELETE FROM t WHERE id = 2});
same_map('t', 'and changes after all of that');

# in a transaction, in order with its rows, and not if rolled back
run('CREATE TABLE x (id integer PRIMARY KEY, a integer); INSERT INTO x VALUES (1, 1), (2, 2)');
run('BEGIN; INSERT INTO x VALUES (3, 3); ALTER TABLE x ALTER COLUMN a TYPE bigint USING a + 100; INSERT INTO x VALUES (4, 4); UPDATE x SET a = 0 WHERE id = 1; COMMIT');
same_map('x', 'rows before and after it in one transaction');
run('BEGIN; ALTER TABLE x ADD COLUMN gone integer; INSERT INTO x VALUES (5, 5, 5); ROLLBACK');
same_map('x', 'rolled back');
run('BEGIN; SAVEPOINT p; ALTER TABLE x ALTER COLUMN a TYPE bigint USING a + 1000; ROLLBACK TO p; INSERT INTO x VALUES (6, 6); COMMIT');
same_map('x', 'rolled back to a savepoint, with a row after');
run('BEGIN; ALTER TABLE x ADD COLUMN p1 integer DEFAULT 1; ALTER TABLE x ADD COLUMN p2 integer DEFAULT 2; COMMIT');
same_map('x', 'two statements in one transaction');
run(q{BEGIN; ALTER TABLE x ALTER COLUMN a TYPE bigint USING a + 5; PREPARE TRANSACTION 'ddl'});
caught_up();
$node->safe_psql('postgres', q{COMMIT PREPARED 'ddl'});
same_map('x', 'a prepared transaction, once committed');

# partitions and inheritance: each table with rows has its own
run('CREATE TABLE p (id integer PRIMARY KEY, a integer) PARTITION BY RANGE (id);
	CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (10);
	CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (10) TO (20);
	INSERT INTO p VALUES (1, 1), (11, 11);
	CREATE TABLE parent (id integer PRIMARY KEY, a integer);
	CREATE TABLE kid (PRIMARY KEY (id)) INHERITS (parent);
	INSERT INTO parent VALUES (1, 1); INSERT INTO kid VALUES (2, 2)');
caught_up();
run('ALTER TABLE p ALTER COLUMN a TYPE bigint USING a + 1');
same_map('p1', 'a partition, by its partitioned table\'s statement');
same_map('p2', 'and another');
run('ALTER TABLE parent ALTER COLUMN a TYPE bigint USING a + 1');
same_map('parent', 'a parent');
same_map('kid', 'and what inherits from it');

# an enum's value renamed: every table reaching it, however, wherever
$node->safe_psql('postgres', q{CREATE TYPE public.shared_e AS ENUM ('one', 'two'); CREATE DOMAIN public.shared_d AS public.shared_e});
run(q{CREATE TABLE direct (id integer PRIMARY KEY, a public.shared_e);
	CREATE TABLE by_domain (id integer PRIMARY KEY, a public.shared_d);
	CREATE TABLE by_array (id integer PRIMARY KEY, a public.shared_e[]);
	CREATE TYPE comp AS (x integer, y public.shared_e);
	CREATE TABLE by_comp (id integer PRIMARY KEY, a comp);
	CREATE TABLE by_row (id integer PRIMARY KEY, a direct);
	CREATE TABLE no_reach (id integer PRIMARY KEY, a text);
	INSERT INTO direct VALUES (1, 'one'); INSERT INTO by_domain VALUES (1, 'one'); INSERT INTO by_array VALUES (1, '{one,two}');
	INSERT INTO by_comp VALUES (1, ROW(1, 'one')); INSERT INTO by_row VALUES (1, ROW(1, 'one')); INSERT INTO no_reach VALUES (1, 'one')});
$node->safe_psql('postgres', "$on2 CREATE SCHEMA s VERSIONED; CREATE TABLE s.elsewhere (id integer PRIMARY KEY, a public.shared_e); INSERT INTO s.elsewhere VALUES (1, 'one')");
caught_up();
my $map_before = $node->safe_psql('postgres', "$on1 SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.no_reach'::regclass::oid");
$node->safe_psql('postgres', q{ALTER TYPE public.shared_e RENAME VALUE 'one' TO 'uno'});
same_map($_, "enum renamed: $_") for qw(direct by_domain by_array by_comp by_row);
same_map('elsewhere', 'enum renamed: on another branch than the session\'s', $on2);
is( $node->safe_psql('postgres', "$on1 SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.no_reach'::regclass::oid"),
	$map_before, 'a table not reaching it has the same map');
$node->safe_psql('postgres', q{ALTER TYPE public.shared_e ADD VALUE 'three'});
same_map('direct', 'an enum value added changes no row');

# told whole with the worker stopped: its order among what's waiting
SKIP:
{
	skip 'no way to stop a process', 1 if $windows_os;
	run('CREATE TABLE late (id integer PRIMARY KEY, a integer)');
	caught_up();
	my $worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	kill 'STOP', $worker;
	run('INSERT INTO late VALUES (1, 1)');
	run('ALTER TABLE late ADD COLUMN b integer DEFAULT 7');
	run('INSERT INTO late VALUES (2, 2, 2)');
	kill 'CONT', $worker;
	same_map('late', 'rows, DDL and rows, read all at once');
}

# a worker stopped having applied it is told it again
SKIP:
{
	skip 'injection points not supported by this build', 1 if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	caught_up();
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-applied', 'wait')});
	run('ALTER TABLE x ALTER COLUMN a TYPE bigint USING a + 7');
	$node->wait_for_event('capture worker', 'capture-applied');
	my $held = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applied')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($held)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $held})
	  or die 'no worker came back';
	run('INSERT INTO x (id, a) VALUES (70, 70)');
	same_map('x', 'told again after applying');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
}

# a statement in a transaction with an older snapshot can't read what's committed now
my ($rc, $out, $err) = $node->psql('postgres', "$on1 BEGIN ISOLATION LEVEL REPEATABLE READ; ALTER TABLE s.x ADD COLUMN rr integer; COMMIT");
like($err, qr/can't take this statement in a transaction above READ COMMITTED/, 'REPEATABLE READ is refused');
same_map('x', 'and nothing came of it');

# holding writers off: renaming an enum's value locks no table, and a writer committing after the
# read and before this commits would come first and be lost
my $writer = $node->background_psql('postgres');
$writer->query_safe("$on1 BEGIN; INSERT INTO s.direct VALUES (80, 'two')");
my $ddl = $node->background_psql('postgres');
$ddl->query_safe('BEGIN');
$ddl->query_until(qr/started/, "\\echo started\nALTER TYPE public.shared_e RENAME VALUE 'two' TO 'dos';\n\\echo finished\n");
$node->poll_query_until('postgres',
	q{SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE 'ALTER TYPE%'})
  or die 'the statement did not wait for the writer';
$writer->query_safe('COMMIT');
$writer->quit;
$ddl->query_safe('SELECT 1');
$ddl->query_safe('COMMIT');
$ddl->quit;
same_map('direct', 'a writer open across an enum\'s value renamed');

# a table there before capture began is held, once told whole
unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal/m, 'nothing crashed');
$node->stop;

$node = PostgreSQL::Test::Cluster->new('ddl_before');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $b3 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$on1 = "SET catalog_branch = $b3;";
$slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
$node->safe_psql('postgres', "$on1 BEGIN; CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (id integer PRIMARY KEY, a integer); INSERT INTO s.t VALUES (1, 1); COMMIT");
$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0}) or die 'no worker';
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_history.capture_tables WHERE nkey IS NULL}), '1', 'a table there before capture began is marked');
run('ALTER TABLE t ADD COLUMN b integer DEFAULT 2');
same_map('t', 'told whole, it has a row map');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_history.capture_tables WHERE nkey IS NULL}), '0', 'and is no longer marked');
run('INSERT INTO t VALUES (2, 2, 2)');
same_map('t', 'and goes on from there');
$node->stop;
done_testing();
