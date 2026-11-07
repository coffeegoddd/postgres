# Copyright (c) 2026, PostgreSQL Global Development Group

# A schema's root value from capture, as of an exact point: every transaction committed before it,
# none after, and with nothing changing, the very root reading the tables makes.

use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $fixture = dirname(__FILE__) . '/../capture_fixture';
my $node = PostgreSQL::Test::Cluster->new('barrier');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $b1 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b2 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on1 = "SET catalog_branch = $b1;";
my $on2 = "SET catalog_branch = $b2;";
$node->safe_psql('postgres', "$on1 CREATE SCHEMA s VERSIONED");
$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0}) or die 'no worker';

sub barrier
{
	my ($schema, $on) = @_;
	return $node->safe_psql('postgres', "$on SELECT test_capture_barrier('$schema')");
}

sub read_root
{
	my ($schema, $on) = @_;
	return $node->safe_psql('postgres', "$on SELECT test_ws_root_capture('$schema')");
}

sub same_root
{
	my ($schema, $on, $what) = @_;
	my $from_capture = barrier($schema, $on);
	isnt($from_capture, '', "$what: a root");
	is($from_capture, read_root($schema, $on), $what);
	return $from_capture;
}

# A barrier in another session, that has written its marker, with the worker stopped.
sub barrier_held
{
	my ($on, $schema) = @_;
	$schema //= 's';
	my $bg = $node->background_psql('postgres');
	$bg->query_until(qr/started/, "\\echo started\n$on SELECT test_capture_barrier('$schema') AS root \\gset\n\\echo :root\n\\echo finished\n");
	$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'CaptureBarrier'}) or die 'no marker';
	return $bg;
}

sub finish
{
	my ($bg) = @_;
	$bg->{stdout} = '';
	$bg->{run}->pump until $bg->{stdout} =~ /finished/ || !$bg->{run}->pumpable;
	return $bg;
}

sub caught_up
{
	my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = current_database()});
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die 'capture did not catch up';
}

sub barrier_result
{
	my ($bg) = @_;
	finish($bg);
	my ($root) = $bg->{stdout} =~ /^([0-9a-f]{40})$/m;
	$bg->quit;
	return $root;
}

same_root('s', $on1, 'a schema with nothing in it');
$node->safe_psql('postgres', "$on1 CREATE TABLE s.t (id integer PRIMARY KEY, v text)");
same_root('s', $on1, 'a table with no rows');
$node->safe_psql('postgres', "$on1 INSERT INTO s.t SELECT g, 'v' || g FROM generate_series(1, 2000) g");
same_root('s', $on1, 'rows');
$node->safe_psql('postgres', "$on1 UPDATE s.t SET v = 'x' WHERE id % 2 = 0; DELETE FROM s.t WHERE id % 5 = 0");
same_root('s', $on1, 'rows changed');
$node->safe_psql('postgres', "$on1 ALTER TABLE s.t ADD COLUMN extra integer DEFAULT 7");
same_root('s', $on1, 'a statement that changed the rows');

# what isn't rows: objects, sequences, foreign keys, partitions (owned by a role that can travel in a
# commit, which one that can log in can't)
$node->safe_psql('postgres', qq{$on1 CREATE ROLE objs; GRANT USAGE, CREATE ON SCHEMA s TO objs; SET ROLE objs; SET search_path = s;
	CREATE SEQUENCE seq; SELECT nextval('seq');
	CREATE TABLE base (id integer PRIMARY KEY);
	CREATE VIEW v AS SELECT id FROM base;
	CREATE FUNCTION f() RETURNS integer LANGUAGE sql AS 'SELECT 1';
	CREATE TABLE parent (id integer PRIMARY KEY);
	CREATE TABLE child (id integer PRIMARY KEY, p integer REFERENCES parent);
	INSERT INTO parent VALUES (1); INSERT INTO child VALUES (1, 1);
	CREATE TABLE part (id integer PRIMARY KEY) PARTITION BY RANGE (id);
	CREATE TABLE part1 PARTITION OF part FOR VALUES FROM (0) TO (10);
	INSERT INTO part VALUES (1)});
same_root('s', $on1, 'objects, sequences, foreign keys and partitions');

# everything the fixture has
$node->safe_psql('postgres', "$on2 CREATE SCHEMA rt VERSIONED; GRANT USAGE, CREATE ON SCHEMA rt TO objs; SET ROLE objs; " . slurp_file("$fixture/load.sql"));
same_root('rt', $on2, 'the fixture');
$node->safe_psql('postgres', "$on2 SET ROLE objs; " . slurp_file("$fixture/dml.sql"));
same_root('rt', $on2, 'and its changes');

# each branch has its own
$node->safe_psql('postgres', "$on2 CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (id integer PRIMARY KEY, v text); INSERT INTO s.t VALUES (1, 'two')");
my $one = same_root('s', $on1, 'one branch');
my $two = same_root('s', $on2, 'another, with the same names');
isnt($one, $two, 'their roots differ');

# the exact point: committed before is in, open or committed after is not
my $open = $node->background_psql('postgres');
$open->query_safe("$on1 BEGIN; INSERT INTO s.t VALUES (900001, 'open')");
$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (900002, 'committed')");
my $worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
SKIP:
{
	skip 'no way to stop a process', 2 if $windows_os;
	kill 'STOP', $worker;
	my $bg = barrier_held($on1);
	$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (900003, 'after the marker')");
	kill 'CONT', $worker;
	my $root = barrier_result($bg);
	ok(defined $root, 'a barrier while the worker was stopped');
	$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id = 900003");
	is($root, read_root('s', $on1), 'committed before is in; open, or committed after, is not');

	# written before the marker, committed after it, is not; committed with the worker behind, is
	my $straddles = $node->background_psql('postgres');
	$straddles->query_safe("$on1 BEGIN; INSERT INTO s.t VALUES (900007, 'straddles')");
	kill 'STOP', $worker;
	$node->safe_psql('postgres', "$on1 ALTER TABLE s.child ADD COLUMN behind integer DEFAULT 3; INSERT INTO s.t VALUES (900008, 'behind')");
	$bg = barrier_held($on1);
	$straddles->query_safe('COMMIT');
	$straddles->quit;
	kill 'CONT', $worker;
	$root = barrier_result($bg);
	$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id = 900007");
	is($root, read_root('s', $on1), 'DDL committed before, with the worker behind, is in; a row committed after is not');
}
$open->query_safe('ROLLBACK');
$open->quit;

# while it waits: rows go on, a change to the catalogs doesn't commit, even of nothing versioned, and
# neither fails
SKIP:
{
	skip 'no way to stop a process', 3 if $windows_os;
	$node->safe_psql('postgres', "CREATE SCHEMA shared_types; CREATE TYPE shared_types.mood AS ENUM ('a')");
	kill 'STOP', $worker;
	my $bg = barrier_held($on1);
	my $ddl = $node->background_psql('postgres');
	$ddl->query_until(qr/started/, "\\echo started\nALTER TYPE shared_types.mood RENAME VALUE 'a' TO 'b';\n\\echo finished\n");
	ok( $node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE query LIKE 'ALTER TYPE shared_types%' AND wait_event_type = 'Lock'}),
		'a shared enum renamed waits');
	$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (900005, 'a row writer')");
	pass('a row writer does not');
	kill 'CONT', $worker;
	ok(defined barrier_result($bg), 'the barrier finishes');
	$ddl->{stdout} = '';
	$ddl->{run}->pump until $ddl->{stdout} =~ /finished/ || !$ddl->{run}->pumpable;
	$ddl->quit;
	is($node->safe_psql('postgres', q{SELECT enumlabel FROM pg_enum e JOIN pg_type t ON t.oid = e.enumtypid WHERE typname = 'mood'}), 'b', 'and the rename then commits');
	$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id = 900005");
}

# DDL of the schema waits to begin; DDL of what a view depends on elsewhere has it locked, so the
# barrier is what gives up, not whoever's committing
SKIP:
{
	skip 'no way to stop a process', 4 if $windows_os;
	$node->safe_psql('postgres', "$on1 SET ROLE objs; CREATE VIEW s.dropped_view AS SELECT 1 AS a");
	kill 'STOP', $worker;
	my $bg = barrier_held($on1);
	my $ddl = $node->background_psql('postgres');
	$ddl->query_until(qr/started/, "\\echo started\n$on1 BEGIN; DROP VIEW s.dropped_view; COMMIT;\n\\echo finished\n");
	$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE query LIKE '%DROP VIEW s.dropped_view%' AND wait_event_type = 'Lock'}) or die 'DDL did not wait';
	kill 'CONT', $worker;
	ok(defined barrier_result($bg), 'a view dropped during a barrier: the barrier finishes');
	finish($ddl);
	$ddl->quit;
	is($node->safe_psql('postgres', "$on1 SELECT to_regclass('s.dropped_view') IS NULL"), 't', 'and so does the drop');

	# a type of the schema is locked first too
	$node->safe_psql('postgres', qq{$on1 SET ROLE objs; CREATE TYPE s.ct AS (a integer); CREATE VIEW s.of_type AS SELECT ROW(1)::s.ct AS c});
	kill 'STOP', $worker;
	$bg = barrier_held($on1);
	$ddl = $node->background_psql('postgres');
	$ddl->query_until(qr/started/, "\\echo started\n$on1 ALTER TYPE s.ct RENAME TO ct2;\n\\echo finished\n");
	$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE query LIKE '%ALTER TYPE s.ct RENAME%' AND wait_event_type = 'Lock'}) or die 'DDL did not wait';
	kill 'CONT', $worker;
	ok(defined barrier_result($bg), 'a type a view uses changed during a barrier: the barrier finishes');
	finish($ddl);
	$ddl->quit;
	is($node->safe_psql('postgres', "$on1 SELECT to_regtype('s.ct2') IS NOT NULL"), 't', 'and so does the change');
	$node->safe_psql('postgres', "$on1 SET ROLE objs; DROP VIEW s.of_type; DROP TYPE s.ct2");

	$node->safe_psql('postgres', "CREATE TABLE shared_types.seen (a integer)");
	$node->safe_psql('postgres', "$on2 CREATE SCHEMA with_view VERSIONED; CREATE VIEW with_view.v AS SELECT a FROM shared_types.seen");
	kill 'STOP', $worker;
	$bg = barrier_held($on2, 'with_view');
	$ddl = $node->background_psql('postgres');

	# (its own deadlock check is put off, so this is the barrier's turn to find it, however slowly this runs)
	$ddl->query_until(qr/started/, "\\echo started\nSET deadlock_timeout = '10min'; ALTER TABLE shared_types.seen ADD COLUMN b integer;\n\\echo finished\n");
	$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE query LIKE '%ADD COLUMN b integer%' AND wait_event_type = 'Lock'}) or die 'DDL did not wait';
	kill 'CONT', $worker;
	finish($bg);
	like($bg->{stderr}, qr/lock timeout/, 'what a view depends on, changing: the barrier gives up');
	$bg->quit;
	finish($ddl);
	$ddl->quit;
	is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_attribute WHERE attrelid = 'shared_types.seen'::regclass AND attname = 'b'}), '1', 'and the commit goes through');
	$node->safe_psql('postgres', "$on2 DROP SCHEMA with_view CASCADE");
}

# a marker told again while its barrier is still building, by a worker that crashed with a commit right
# after the marker applied: what it saved, and what others saved, is left, and so the root's right
SKIP:
{
	skip 'injection points not supported by this build', 6 if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	same_root('s', $on1, 'with nothing held');
	my $saved = qq{SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name LIKE 'capture/barrier/%/' || 's.t'::regclass::oid};
	my $straddles = $node->background_psql('postgres');
	$straddles->query_safe("$on1 BEGIN; INSERT INTO s.t VALUES (900006, 'right after the marker')");
	kill 'STOP', $worker;
	my ($rc1, $out1, $err1) = $node->psql('postgres', "$on2 SELECT test_capture_barrier('s', 100)");
	like($err1, qr/capture did not reach/, 'one barrier gives up, on another branch');
	my $bg = barrier_held($on1);
	$straddles->query_safe('COMMIT');
	$straddles->quit;
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-applied', 'wait'); SELECT injection_points_attach('capture-barrier-saved', 'wait')});
	kill 'CONT', $worker;
	$node->wait_for_event('capture worker', 'capture-applied');
	$node->wait_for_event('client backend', 'capture-barrier-saved');
	my $before = $node->safe_psql('postgres', "$on1 $saved");
	isnt($before, '', 'a barrier has its maps, with a commit right after its marker applied, not confirmed');
	isnt($before, $node->safe_psql('postgres', qq{$on1 SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.t'::regclass::oid}),
		'what it saved is not the map now');
	ok( $node->safe_psql('postgres', qq{SELECT confirmed_flush_lsn < (SELECT max(lsn) FROM pg_history.capture_barriers) FROM pg_replication_slots WHERE slot_name LIKE 'prolly_capture_%'}) eq 't',
		'its marker is to be told again');
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applied')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($worker)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $worker})
	  or die 'no worker came back';
	$worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id = 900006");
	caught_up();
	is($node->safe_psql('postgres', "$on1 $saved"), $before, 'and told again, it is left as it was');
	$node->safe_psql('postgres', q{SELECT injection_points_wakeup('capture-barrier-saved'); SELECT injection_points_detach('capture-barrier-saved')});
	is(barrier_result($bg), read_root('s', $on1), 'and the root does not have what came after');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
}

# a schema that's another by the time its relations are locked
SKIP:
{
	skip 'injection points not supported by this build', 1 if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', "CREATE EXTENSION injection_points; SELECT injection_points_attach('capture-barrier-locked', 'wait')");
	$node->safe_psql('postgres', "$on1 CREATE SCHEMA renames VERSIONED");
	my $bg = $node->background_psql('postgres', on_error_stop => 0);
	$bg->query_until(qr/started/, "\\echo started\n$on1 SELECT test_capture_barrier('renames');\n\\echo finished\n");
	$node->wait_for_event('client backend', 'capture-barrier-locked');
	$node->safe_psql('postgres', "$on1 ALTER SCHEMA renames RENAME TO renamed; CREATE SCHEMA renames VERSIONED");
	$node->safe_psql('postgres', "SELECT injection_points_wakeup('capture-barrier-locked'); SELECT injection_points_detach('capture-barrier-locked')");
	finish($bg);
	like($bg->{stderr}, qr/schema "renames" changed during a commit from capture/, 'a schema renamed under a barrier');
	$bg->quit;
	$node->safe_psql('postgres', "$on1 DROP SCHEMA renames; DROP SCHEMA renamed; DROP EXTENSION injection_points");
}

# what it takes: rights tried before anything is written, its own reads on a pinned path, rows no
# role can have some of
$node->safe_psql('postgres', qq{$on1 CREATE ROLE nobody_much;
	GRANT EXECUTE ON FUNCTION test_capture_barrier(text, integer) TO nobody_much;
	GRANT USAGE ON SCHEMA s TO nobody_much});
caught_up();
my $markers = q{SELECT string_agg(lsn::text, ',') FROM pg_history.capture_barriers};
my $markers_were = $node->safe_psql('postgres', $markers);
my ($rcr, $outr, $errr) = $node->psql('postgres', "$on1 SET ROLE nobody_much; SELECT test_capture_barrier('s')");
like($errr, qr/permission denied for schema pg_history/, 'a role that cannot read what capture saved');
$node->safe_psql('postgres', "$on1 REVOKE ALL ON SCHEMA s FROM nobody_much; GRANT USAGE ON SCHEMA pg_history TO nobody_much");
($rcr, $outr, $errr) = $node->psql('postgres', "$on1 SET ROLE nobody_much; SELECT test_capture_barrier('s')");
like($errr, qr/permission denied for schema s/, 'nor one that has no use of the schema');
$node->safe_psql('postgres', "$on1 GRANT USAGE ON SCHEMA s TO nobody_much");
($rcr, $outr, $errr) = $node->psql('postgres', "$on1 SET ROLE nobody_much; SELECT test_capture_barrier('s')");
like($errr, qr/permission denied for (table|view|sequence)/, 'a role that cannot read a table');
$node->safe_psql('postgres', "$on1 GRANT SELECT ON ALL TABLES IN SCHEMA s TO nobody_much; REVOKE SELECT ON ALL SEQUENCES IN SCHEMA s FROM nobody_much");
($rcr, $outr, $errr) = $node->psql('postgres', "$on1 SET ROLE nobody_much; SELECT test_capture_barrier('s')");
like($errr, qr/permission denied for sequence/, 'nor a sequence');
$node->safe_psql('postgres', qq{$on1 SET ROLE objs; CREATE TABLE s.guarded (id integer PRIMARY KEY); ALTER TABLE s.guarded ENABLE ROW LEVEL SECURITY; RESET ROLE; GRANT SELECT ON ALL TABLES IN SCHEMA s TO nobody_much; GRANT SELECT ON ALL SEQUENCES IN SCHEMA s TO nobody_much});
($rcr, $outr, $errr) = $node->psql('postgres', "$on1 SET ROLE nobody_much; SELECT test_capture_barrier('s')");
like($errr, qr/role row security applies to/, 'a role row security applies to');
$node->safe_psql('postgres', "$on1 SET ROLE objs; DROP TABLE s.guarded");
caught_up();
is($node->safe_psql('postgres', $markers), $markers_were, 'none of them wrote a marker');
$node->safe_psql('postgres', "$on1 REVOKE ALL ON ALL TABLES IN SCHEMA s FROM nobody_much; REVOKE ALL ON ALL SEQUENCES IN SCHEMA s FROM nobody_much; REVOKE ALL ON SCHEMA s FROM nobody_much");

$node->safe_psql('postgres', q{CREATE SCHEMA evil;
	CREATE FUNCTION evil.same(pg_lsn, pg_lsn) RETURNS boolean LANGUAGE plpgsql AS $$BEGIN RAISE WARNING 'ran as a barrier read'; RETURN true; END$$;
	CREATE OPERATOR evil.= (FUNCTION = evil.same, LEFTARG = pg_lsn, RIGHTARG = pg_lsn)});
($rcr, $outr, $errr) = $node->psql('postgres', "$on1 SET search_path = evil, pg_catalog, public; SELECT test_capture_barrier('s')");
unlike($errr, qr/ran as a barrier read/, 'an operator ahead on the search path');
like($outr, qr/^[0-9a-f]{40}$/m, 'and the barrier gave a root');
$node->safe_psql('postgres', 'DROP SCHEMA evil CASCADE');

# a table is its object id, not its name: two swapping names, with the worker going on and starting
# again between, and a root that's each table's rows by the name it has now
$node->safe_psql('postgres', "$on1 SET ROLE objs; CREATE TABLE s.swap_a (id integer PRIMARY KEY, v text); CREATE TABLE s.swap_b (id integer PRIMARY KEY, v text);
	INSERT INTO s.swap_a VALUES (1, 'was a'); INSERT INTO s.swap_b VALUES (1, 'was b'), (2, 'was b')");
same_root('s', $on1, 'two tables');
$node->safe_psql('postgres', "$on1 SET ROLE objs; BEGIN; ALTER TABLE s.swap_a RENAME TO swap_tmp; ALTER TABLE s.swap_b RENAME TO swap_a; ALTER TABLE s.swap_tmp RENAME TO swap_b; COMMIT");
same_root('s', $on1, 'their names swapped');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($worker)");
$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $worker})
  or die 'no worker came back';
$worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
$node->safe_psql('postgres', "$on1 SET ROLE objs; BEGIN; ALTER TABLE s.swap_a RENAME TO swap_tmp; ALTER TABLE s.swap_b RENAME TO swap_a; ALTER TABLE s.swap_tmp RENAME TO swap_b; COMMIT;
	INSERT INTO s.swap_a VALUES (3, 'after')");
same_root('s', $on1, 'swapped back across a worker starting again, and written');
is($node->safe_psql('postgres', "$on1 SELECT string_agg(v, ',' ORDER BY id) FROM s.swap_a"), 'was a,after', 'by the names they have now');

# a table dropped and another made, which may have its object id: the new one's rows are told whole
$node->safe_psql('postgres', "$on1 SET ROLE objs; DROP TABLE s.swap_b; CREATE TABLE s.swap_b (id integer PRIMARY KEY, v text); INSERT INTO s.swap_b VALUES (9, 'new')");
same_root('s', $on1, 'a table dropped and one made in its name');
$node->safe_psql('postgres', "$on1 SET ROLE objs; DROP TABLE s.swap_a");

# a table dropped above READ COMMITTED has its fingerprint forgotten too, or another made later with
# its object id, and the same columns, would be taken for it, its rows never told
my $dropped = $node->safe_psql('postgres', "$on1 SELECT 's.swap_b'::regclass::oid");
$node->safe_psql('postgres', "$on1 SET ROLE objs; BEGIN ISOLATION LEVEL REPEATABLE READ; DROP TABLE s.swap_b; COMMIT");
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_history.capture_fingerprints WHERE relid = $dropped"), '0',
	'dropped in REPEATABLE READ, its fingerprint is forgotten');
# a barrier's marker is flushed as it's written, so that the worker can read it at once, rather than
# when something else flushes the WAL: with the worker stopped, what's flushed while the barrier
# waits is past its marker
SKIP:
{
	skip 'no way to stop a process', 2 if $windows_os;
	kill 'STOP', $worker;
	my $bg = barrier_held($on1);
	my $flushed = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	kill 'CONT', $worker;
	isnt(barrier_result($bg), undef, 'a barrier with its marker');
	is($node->safe_psql('postgres', "SELECT max(lsn) <= '$flushed'::pg_lsn FROM pg_history.capture_barriers"), 't',
		'its marker flushed before the worker read it');
}

# a session's own changes, committed, are in the root its next commit is made of, made at once after
# them: with the worker stopped, so that the barrier has to wait for them
SKIP:
{
	skip 'no way to stop a process', 1 if $windows_os;
	my $own = $node->background_psql('postgres');
	kill 'STOP', $worker;
	$own->query_safe("$on1 INSERT INTO s.t VALUES (900011, 'my own')");
	$own->query_until(qr/started/, "\\echo started\n$on1 SELECT test_capture_barrier('s') AS root \\gset\n\\echo :root\n\\echo finished\n");
	$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'CaptureBarrier'}) or die 'no marker';
	kill 'CONT', $worker;
	my ($root) = finish($own)->{stdout} =~ /^([0-9a-f]{40})$/m;
	$own->quit;
	is($root, read_root('s', $on1), "a session's own committed change is in the root of its commit");
	$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id = 900011");
}

# its own transaction: nothing it's written, and nothing older than now
my ($rc, $out, $err) = $node->psql('postgres', "$on1 BEGIN; INSERT INTO s.t VALUES (900004, 'x'); SELECT test_capture_barrier('s'); ROLLBACK");
like($err, qr/can't run in a transaction that has written anything/, 'not in a transaction that has written');
($rc, $out, $err) = $node->psql('postgres', "$on1 BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT test_capture_barrier('s'); ROLLBACK");
like($err, qr/can't run above READ COMMITTED/, 'not above READ COMMITTED');
($rc, $out, $err) = $node->psql('postgres', q{CREATE SCHEMA pub; SELECT test_capture_barrier('pub')});
like($err, qr/is not versioned/, 'not of a shared schema');

# a table that can't be logged can't be there to be left out
($rc, $out, $err) = $node->psql('postgres', "$on1 ALTER TABLE s.t SET UNLOGGED");
like($err, qr/unlogged/, 'a versioned table can not become unlogged');

# a table with no key, with rows the same as each other
$node->safe_psql('postgres', "$on1 SET ROLE objs; CREATE TABLE s.nokey (a integer, b text); INSERT INTO s.nokey SELECT g % 3, 'x' FROM generate_series(1, 30) g; DELETE FROM s.nokey WHERE ctid = '(0,1)'");
same_root('s', $on1, 'a table with no key');
$node->safe_psql('postgres', "$on1 SET ROLE objs; DROP TABLE s.nokey");

# a table it holds no map of
$node->safe_psql('postgres', "$on1 CREATE SCHEMA nomap VERSIONED; CREATE TABLE nomap.t (a integer PRIMARY KEY)");
same_root('nomap', $on1, 'a table it has a map of');
$node->safe_psql('postgres', "$on1 SET allow_system_table_mods = on; DELETE FROM pg_history.pointers WHERE name = 'capture/map/' || 'nomap.t'::regclass::oid");
($rc, $out, $err) = $node->psql('postgres', "$on1 SELECT test_capture_barrier('nomap')");
like($err, qr/capture holds no row map of table "t"/, 'and one it has not');
$node->safe_psql('postgres', "$on1 DROP SCHEMA nomap CASCADE");

# what it saved stays until the next, in an open transaction or not, and doesn't hold capture up
same_root('s', $on1, 'one more');
my $refs = q{SELECT count(DISTINCT split_part(name, '/', 3) || '/' || split_part(name, '/', 4)) FROM pg_history.pointers WHERE name LIKE 'capture/barrier/%'};
is($node->safe_psql('postgres', $refs), '1', 'only the last has its maps kept');
is($node->safe_psql('postgres', 'SELECT count(*) FROM pg_history.capture_barriers'), '1', 'and its record');

# one in an open transaction has chunks another may want: that's an error, to try again, and doesn't
# hold capture up
my $held_open = $node->background_psql('postgres');
$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (900009, 'new chunks')");
$held_open->query_safe("$on1 BEGIN; SELECT test_capture_barrier('s')");
my ($rco, $outo, $erro) = $node->psql('postgres', "$on1 SELECT test_capture_barrier('s')");
like($erro, qr/lock timeout/, 'a barrier beside one in an open transaction gives up');
$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (900010, 'capture goes on')");
caught_up();
pass('capture does not wait for it');
$held_open->query_safe('COMMIT');
$held_open->quit;
$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id >= 900009");
same_root('s', $on1, 'and tried again once that has committed');

# all the way: from rows written, to a commit from capture, to a schema restored from that commit
$node->safe_psql('postgres', qq{$on1 SET ROLE objs; CREATE TABLE s.commit_me (id integer PRIMARY KEY, v text); INSERT INTO s.commit_me VALUES (1, 'committed')});
# (a commit marks the roles it holds, as whatever makes commits is to)
$node->safe_psql('postgres', q{SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['objs']))});
my $before_commit = read_root('s', $on1);
$node->safe_psql('postgres', "$on1 CREATE SCHEMA from_commit VERSIONED");
my $commit = $node->safe_psql('postgres', qq{$on1 SELECT test_capture_commit('s', 'the first commit from capture', 'from_commit')});
like((split /\n/, $commit)[-1], qr/^[0-9a-f]{40}$/, 'a commit from capture');
is(read_root('from_commit', $on1), $before_commit, 'restored from the commit, a schema captures as what was committed');
is($node->safe_psql('postgres', "$on1 SELECT v FROM from_commit.commit_me"), 'committed', 'and has its rows');
$node->safe_psql('postgres', "$on1 DROP SCHEMA from_commit CASCADE");

# with the worker stopped, a barrier gives up, and lets go of what it held off
SKIP:
{
	skip 'no way to stop a process', 2 if $windows_os;
	kill 'STOP', $worker;
	($rc, $out, $err) = $node->psql('postgres', "$on1 SELECT test_capture_barrier('s', 500)");
	like($err, qr/capture did not reach/, 'a barrier gives up in time');
	is($node->safe_psql('postgres', q{SELECT locktype FROM pg_locks WHERE locktype = 'object' AND objsubid = 2 AND mode = 'ExclusiveLock'}), '', 'and holds nothing');
	kill 'CONT', $worker;
	$node->safe_psql('postgres', "$on1 CREATE TABLE s.after_give_up (id integer PRIMARY KEY)");
	same_root('s', $on1, 'and catalog changes and barriers go on');
}

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal/m, 'nothing crashed');
# waiting for capture to reach a point: once it returns, the map has what was committed before it
my $map_t = q{SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.t'::regclass::oid};
$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (900031, 'waited for')");
my $at = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
$node->safe_psql('postgres', "SELECT test_capture_wait('$at')");
is($node->safe_psql('postgres', "$on1 $map_t"), $node->safe_psql('postgres', "$on1 SELECT test_ws_rows_hash('s', 't')"),
	'waited for capture, the map has what was committed before');
SKIP:
{
	skip 'no way to stop a process', 4 if $windows_os;
	$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id = 900031");
	$at = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	kill 'STOP', $worker;
	my ($rc_w, $o_w, $err_w) = $node->psql('postgres', "SELECT test_capture_wait('$at', 300)");
	like($err_w, qr/capture did not reach [0-9A-F]+\/[0-9A-F]+ in time\nDETAIL:  It has reached [0-9A-F]+\/[0-9A-F]+\./, 'not reached in time, an error saying how far it has');
	my $bg = $node->background_psql('postgres');
	$bg->query_until(qr/started/, "\\echo started\nSELECT test_capture_wait('$at');\n\\echo waited\n");
	ok($node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'CaptureWait'}),
		'waiting for capture, as it is stopped');
	kill 'CONT', $worker;
	$bg->{stdout} = '';
	$bg->{run}->pump until $bg->{stdout} =~ /waited/ || !$bg->{run}->pumpable;
	like($bg->{stdout}, qr/waited/, 'and done once it goes on');
	$bg->quit;
	is($node->safe_psql('postgres', "$on1 $map_t"), $node->safe_psql('postgres', "$on1 SELECT test_ws_rows_hash('s', 't')"),
		'with what was committed before in the map');
}
$node->safe_psql('postgres', 'CREATE DATABASE uncaptured');
$node->safe_psql('uncaptured', 'CREATE EXTENSION test_prolly');
my ($rc_u, $o_u, $err_u) = $node->psql('uncaptured', "SELECT test_capture_wait('0/1')");
like($err_u, qr/database "uncaptured" is not being captured/, 'waiting in a database with nothing captured, an error');

# a transaction left out of capture, as version control's own writes are: all of it, even what it
# wrote before saying so, and only it, the session's next ones captured as ever, after a commit and
# after a rollback
{
	my $map_s = q{SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.t'::regclass::oid};
	my $rows_s = q{SELECT test_ws_rows_hash('s', 't')};
	my $vc = $node->background_psql('postgres');
	$vc->query_safe("$on1 BEGIN; INSERT INTO s.t VALUES (900041, 'left out'); SELECT test_capture_leave_out(); COMMIT");
	caught_up();
	isnt($node->safe_psql('postgres', "$on1 $map_s"), $node->safe_psql('postgres', "$on1 $rows_s"), 'a transaction left out of capture');
	$vc->query_safe("INSERT INTO s.t VALUES (900042, 'captured after a commit')");
	$vc->query_safe("BEGIN; SELECT test_capture_leave_out(); ROLLBACK");
	$vc->query_safe("INSERT INTO s.t VALUES (900043, 'captured after a rollback')");
	$vc->query_safe("BEGIN; SELECT test_capture_leave_out(); DELETE FROM s.t WHERE id = 900041; COMMIT");
	$vc->quit;
	caught_up();
	is($node->safe_psql('postgres', "$on1 $map_s"), $node->safe_psql('postgres', "$on1 $rows_s"),
		"and only it: the session's next ones captured, and the row left out again on its way out");
	$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id IN (900042, 900043)");
}

# a crash between a commit's marker and capture reaching it: the commit is lost, and nothing of it is
# seen; the next commit has what the lost one would have had
SKIP:
{
	skip 'no way to stop a process', 4 if $windows_os;
	$node->safe_psql('postgres', 'ALTER SYSTEM SET restart_after_crash = on; SELECT pg_reload_conf()');
	$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (900021, 'before the marker')");
	my $prev = $node->safe_psql('postgres', q{SELECT coalesce(max(lsn), '0/0') FROM pg_history.capture_barriers});
	kill 'STOP', $worker;
	my $bg = barrier_held($on1);
	kill 'KILL', $worker;
	eval { $bg->quit };
	$node->poll_query_until('postgres', 'SELECT true') or die 'the server did not come back';
	$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker'})
	  or die 'no worker came back';
	$worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	caught_up();
	is( $node->safe_psql('postgres', "SELECT (SELECT count(*) FROM pg_history.capture_barriers WHERE lsn > '$prev')
			+ (SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'capture/barrier/%'
				AND (split_part(name, '/', 3) || '/' || split_part(name, '/', 4))::pg_lsn > '$prev')"), '0',
		"a commit whose marker capture hadn't reached, lost in a crash, isn't seen, capture past its marker");
	$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (900022, 'after the crash')");
	is( $node->safe_psql('postgres', "$on1 SELECT count(*) FROM s.t WHERE id IN (900021, 900022)"), '2',
		'what was committed before the marker is still there');
	same_root('s', $on1, 'and the next commit has it');
	$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id IN (900021, 900022)");
}

$node->stop;
done_testing();

# (a failure with the worker stopped would leave the server unable to stop)
END
{
	kill 'CONT', $worker if $worker && !$windows_os;
}
