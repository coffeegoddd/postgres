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
	my $marker_at = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	kill 'STOP', $worker;
	my $bg = $node->background_psql('postgres');
	$bg->query_until(qr/started/, "\\echo started\n$on1 SELECT test_capture_barrier('s') AS root \\gset\n\\echo :root\n\\echo finished\n");
	$node->poll_query_until('postgres', qq{SELECT pg_current_wal_flush_lsn() > '$marker_at'::pg_lsn}) or die 'no marker';
	$node->safe_psql('postgres', "$on1 INSERT INTO s.t VALUES (900003, 'after the marker')");
	kill 'CONT', $worker;
	my $root = '';
	$bg->{stdout} = '';
	$bg->{run}->pump until $bg->{stdout} =~ /finished/ || !$bg->{run}->pumpable;
	($root) = $bg->{stdout} =~ /^([0-9a-f]{40})$/m;
	$bg->quit;
	ok(defined $root, 'a barrier while the worker was stopped');
	$node->safe_psql('postgres', "$on1 DELETE FROM s.t WHERE id = 900003");
	is($root, read_root('s', $on1), 'committed before is in; open, or committed after, is not');
}
$open->query_safe('ROLLBACK');
$open->quit;

# its own transaction: nothing it's written, and nothing older than now
my ($rc, $out, $err) = $node->psql('postgres', "$on1 BEGIN; INSERT INTO s.t VALUES (900004, 'x'); SELECT test_capture_barrier('s'); ROLLBACK");
like($err, qr/can't run in a transaction that has written anything/, 'not in a transaction that has written');
($rc, $out, $err) = $node->psql('postgres', "$on1 BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT test_capture_barrier('s'); ROLLBACK");
like($err, qr/can't run above READ COMMITTED/, 'not above READ COMMITTED');
($rc, $out, $err) = $node->psql('postgres', q{CREATE SCHEMA pub; SELECT test_capture_barrier('pub')});
like($err, qr/is not versioned/, 'not of a shared schema');

# a table it holds no map of, which a commit can't hold yet
$node->safe_psql('postgres', "$on1 CREATE SCHEMA nokey VERSIONED; CREATE TABLE nokey.t (a integer)");
($rc, $out, $err) = $node->psql('postgres', "$on1 SELECT test_capture_barrier('nokey')");
like($err, qr/capture holds no row map of table "t"/, 'a table with no primary key');
$node->safe_psql('postgres', "$on1 DROP SCHEMA nokey CASCADE");

# what it saved goes with it, and one given up on is gone at the next
same_root('s', $on1, 'one more');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'capture/barrier/%'}), '0', 'nothing left of them');
is($node->safe_psql('postgres', 'SELECT count(*) FROM pg_history.capture_barriers'), '0', 'nor their records');


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
	kill 'CONT', $worker;
	$node->safe_psql('postgres', "$on1 CREATE TABLE s.after_give_up (id integer PRIMARY KEY)");
	same_root('s', $on1, 'and catalog changes and barriers go on');
}

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal/m, 'nothing crashed');
$node->stop;
done_testing();
