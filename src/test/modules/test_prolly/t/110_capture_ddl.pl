# Copyright (c) 2026, PostgreSQL Global Development Group

# What changes a table's rows and tells no change of them, however it does: as it commits, its rows
# go in the WAL, and a table's row map is still, to the byte, the one layer 4 makes by reading it.

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

# the last column dropped and added again, so that its place in the row is the same: only its
# attnum says it isn't the column it was
run(q{CREATE TABLE same_place (id integer PRIMARY KEY, a integer); INSERT INTO same_place VALUES (1, 10)});
caught_up();
run('ALTER TABLE same_place DROP COLUMN a, ADD COLUMN a integer');
same_map('same_place', 'the last column dropped and added again');

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

# one with an older snapshot can't read what's committed now
my ($rc, $out, $err) = $node->psql('postgres', "$on1 BEGIN ISOLATION LEVEL REPEATABLE READ; ALTER TABLE s.x ADD COLUMN rr integer; COMMIT");
like($err, qr/can't commit this above READ COMMITTED/, 'REPEATABLE READ is refused');
same_map('x', 'and nothing came of it');

# holding writers off: renaming an enum's value locks no table, and a writer committing after the
# read and before this commits would come first and be lost
my $writer = $node->background_psql('postgres');
$writer->query_safe("$on1 BEGIN; INSERT INTO s.direct VALUES (80, 'two')");
my $ddl = $node->background_psql('postgres');
$ddl->query_safe("BEGIN; ALTER TYPE public.shared_e RENAME VALUE 'two' TO 'dos'");
$ddl->query_until(qr/started/, "\\echo started\nCOMMIT;\n\\echo finished\n");
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE 'COMMIT%'})
  or die 'the commit did not wait for the writer';
$writer->query_safe('COMMIT');
$writer->quit;
$ddl->quit;
same_map('direct', 'a writer open across an enum\'s value renamed');

# two at once: each is told the other's, whichever commits last
$node->safe_psql('postgres', q{CREATE TYPE public.e1 AS ENUM ('a'); CREATE TYPE public.e2 AS ENUM ('b')});
run(q{CREATE TABLE both_e (id integer PRIMARY KEY, x public.e1, y public.e2); INSERT INTO both_e VALUES (1, 'a', 'b')});
caught_up();
my $t1 = $node->background_psql('postgres');
my $t2 = $node->background_psql('postgres');
$t1->query_safe(q{BEGIN; ALTER TYPE public.e1 RENAME VALUE 'a' TO 'a1'});
$t2->query_safe(q{BEGIN; ALTER TYPE public.e2 RENAME VALUE 'b' TO 'b2'});
$t1->query_safe('COMMIT');
$t2->query_safe('COMMIT');
$t1->quit;
$t2->quit;
same_map('both_e', 'two enums renamed at once');

# a table made, and left open, across a value renamed
$node->safe_psql('postgres', q{CREATE TYPE public.e3 AS ENUM ('c')});
my $open = $node->background_psql('postgres');
$open->query_safe("$on1 BEGIN; CREATE TABLE s.in_flight (id integer PRIMARY KEY, a public.e3); INSERT INTO s.in_flight VALUES (1, 'c')");
$node->safe_psql('postgres', q{ALTER TYPE public.e3 RENAME VALUE 'c' TO 'c3'});
$open->query_safe('COMMIT');
$open->quit;
same_map('in_flight', 'a table made open across a value renamed');

# types changed with no rewrite, so no new file: only the type says
for my $case ([ 'cidr', 'inet', q{'10.0.0.0/8'} ], [ 'integer', 'oid', '7' ], [ 'oid', 'regclass', '1259' ], [ 'timestamp', 'timestamptz', q{'2026-01-02 03:04:05'} ])
{
	my ($from, $to, $v) = @$case;
	run("CREATE TABLE ty_$from (id integer PRIMARY KEY, a $from); INSERT INTO ty_$from VALUES (1, $v)");
	caught_up();
	run("ALTER TABLE ty_$from ALTER COLUMN a TYPE $to");
	same_map("ty_$from", "$from retyped to $to");
}

# a row type that changes, of a table or a composite, used for a column elsewhere
run(q{CREATE TABLE row_of (id integer PRIMARY KEY, a integer);
	CREATE TABLE uses_row (id integer PRIMARY KEY, r row_of);
	INSERT INTO uses_row VALUES (1, ROW(1, 2));
	CREATE TYPE ca AS (a integer);
	CREATE TABLE uses_comp (id integer PRIMARY KEY, c ca);
	INSERT INTO uses_comp VALUES (1, ROW(1))});
caught_up();
run('ALTER TABLE row_of ADD COLUMN b integer');
same_map('uses_row', 'a column added to a table used as a type');
run('ALTER TABLE row_of DROP COLUMN b');
same_map('uses_row', 'and dropped');
run('CREATE TABLE p_of (id integer PRIMARY KEY, a integer) PARTITION BY RANGE (id);
	CREATE TABLE uses_p (id integer PRIMARY KEY, r p_of);
	INSERT INTO uses_p VALUES (1, ROW(1, 2))');
caught_up();
run('ALTER TABLE p_of ADD COLUMN b integer');
same_map('uses_p', 'a partitioned table used as a type');

# an attribute dropped and added again, of a composite or a table's row type: the same type, at
# another place
run(q{CREATE TYPE c1 AS (a integer, b integer); CREATE TABLE uses_c1 (id integer PRIMARY KEY, c c1); INSERT INTO uses_c1 VALUES (1, ROW(1, 2))});
caught_up();
run('ALTER TYPE c1 DROP ATTRIBUTE b; ALTER TYPE c1 ADD ATTRIBUTE b integer');
same_map('uses_c1', 'a composite\'s attribute dropped and added again');
run('ALTER TABLE row_of ADD COLUMN z integer');
run('INSERT INTO uses_row VALUES (5, ROW(5, 5, 5))');
caught_up();
run('ALTER TABLE row_of DROP COLUMN z; ALTER TABLE row_of ADD COLUMN z integer');
same_map('uses_row', 'a table\'s row type\'s column dropped and added again');

# through a range and a multirange, whose values print their subtype's
run(q{CREATE TYPE re AS ENUM ('r1', 'r2'); CREATE FUNCTION rediff(re, re) RETURNS float8 LANGUAGE sql IMMUTABLE AS 'SELECT 1::float8';
	CREATE TYPE re_range AS RANGE (subtype = re, multirange_type_name = re_multi);
	CREATE TABLE by_range (id integer PRIMARY KEY, r re_range, m re_multi);
	INSERT INTO by_range VALUES (1, re_range('r1', 'r2'), re_multi(re_range('r1', 'r2')))});
caught_up();
run(q{ALTER TYPE re RENAME VALUE 'r1' TO 'r0'});
same_map('by_range', 'an enum renamed, through a range and a multirange');

# labels that, with no lengths, print the same in a fingerprint: a and "b <o2>=c", then "a <o2>=b" and c
run(q{CREATE TYPE tricky AS ENUM ('l1', 'l2'); CREATE TABLE by_tricky (id integer PRIMARY KEY, x tricky); INSERT INTO by_tricky VALUES (1, 'l1')});
caught_up();
my $o2 = $node->safe_psql('postgres', "$on1 SELECT oid FROM pg_enum WHERE enumtypid = 's.tricky'::regtype AND enumlabel = 'l2'");
run(qq{BEGIN; ALTER TYPE tricky RENAME VALUE 'l1' TO 'a'; ALTER TYPE tricky RENAME VALUE 'l2' TO 'b $o2=c'; COMMIT});
same_map('by_tricky', 'a label holding what looks like another');
run(qq{BEGIN; ALTER TYPE tricky RENAME VALUE 'a' TO 'a $o2=b'; ALTER TYPE tricky RENAME VALUE 'b $o2=c' TO 'c'; COMMIT});
same_map('by_tricky', 'and labels that would print as the same');

# a catalog changed by hand, as the only command of its transaction
$node->safe_psql('postgres', q{CREATE TYPE public.eh AS ENUM ('h')});
run(q{CREATE TABLE by_hand (id integer PRIMARY KEY, a public.eh); INSERT INTO by_hand VALUES (1, 'h')});
caught_up();
$node->safe_psql('postgres', q{UPDATE pg_enum SET enumlabel = 'h2' WHERE enumtypid = 'public.eh'::regtype});
same_map('by_hand', 'a lone UPDATE of a catalog');

# a statement over a replication connection commits through the same place
$node->safe_psql('postgres', q{CREATE TYPE public.ew AS ENUM ('w')});
run(q{CREATE TABLE by_walsender (id integer PRIMARY KEY, a public.ew); INSERT INTO by_walsender VALUES (1, 'w')});
caught_up();
$node->psql('postgres', q{ALTER TYPE public.ew RENAME VALUE 'w' TO 'w2'}, connstr => $node->connstr('postgres') . ' replication=database');
is($node->safe_psql('postgres', q{SELECT enumlabel FROM pg_enum WHERE enumtypid = 'public.ew'::regtype}), 'w2', 'a walsender renamed a value');
same_map('by_walsender', 'a walsender\'s statement');

# dropping a temporary table ON COMMIT takes a column with it, as this is committing
my $temp = $node->background_psql('postgres');
$temp->query_safe(qq{$on1 BEGIN; CREATE TEMP TABLE gone_at_commit (a integer) ON COMMIT DROP;
	CREATE TABLE s.uses_temp (id integer PRIMARY KEY, a integer, t gone_at_commit); INSERT INTO s.uses_temp VALUES (1, 1, ROW(1))});
$temp->query_safe('COMMIT');
$temp->quit;
same_map('uses_temp', 'a column dropped by an ON COMMIT DROP');

# a transaction left open with a table locked doesn't hold up an unrelated commit
my $idle = $node->background_psql('postgres');
$idle->query_safe("$on1 BEGIN; LOCK TABLE s.x IN ACCESS EXCLUSIVE MODE");
$node->safe_psql('postgres', 'SET statement_timeout = 0; CREATE TEMP TABLE unrelated (a integer)', timeout => 60);
pass('a catalog change commits past a table locked and left open');
$idle->query_safe('ROLLBACK');
$idle->quit;

# a role can't have what's run at commit be its own
$node->safe_psql('postgres', q{CREATE ROLE evil LOGIN; CREATE SCHEMA evil AUTHORIZATION evil;
	CREATE TABLE evil.owned_by_evil ()});
$node->safe_psql('postgres', q{SET ROLE evil;
	CREATE FUNCTION evil.oid_eq(oid, oid) RETURNS boolean LANGUAGE plpgsql AS $$
		BEGIN EXECUTE 'ALTER ROLE evil SUPERUSER'; RETURN pg_catalog.oideq($1, $2); END $$;
	CREATE OPERATOR evil.= (LEFTARG = oid, RIGHTARG = oid, FUNCTION = evil.oid_eq);
	CREATE FUNCTION evil.text_eq(text, text) RETURNS boolean LANGUAGE plpgsql AS $$
		BEGIN EXECUTE 'ALTER ROLE evil SUPERUSER'; RETURN pg_catalog.texteq($1, $2); END $$;
	CREATE OPERATOR evil.= (LEFTARG = text, RIGHTARG = text, FUNCTION = evil.text_eq);
	SET search_path = evil, pg_catalog;
	CREATE TEMP TABLE trigger_a_compare (a integer)});
is($node->safe_psql('postgres', q{SELECT rolsuper FROM pg_roles WHERE rolname = 'evil'}), 'f', 'a role can\'t make itself a superuser');

# prepared and rolled back: nothing
run(q{BEGIN; ALTER TABLE x ALTER COLUMN a TYPE bigint USING a + 11; PREPARE TRANSACTION 'gone'});
$node->safe_psql('postgres', q{ROLLBACK PREPARED 'gone'});
same_map('x', 'prepared and rolled back');

# what's dropped takes columns with it, by no ALTER TABLE
run(q{CREATE DOMAIN dd AS integer; CREATE TYPE dt AS ENUM ('z'); CREATE TYPE dc AS (x dt);
	CREATE FUNCTION df(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1';
	CREATE TABLE dropped (id integer PRIMARY KEY, a dd, b dt, c dc, g integer GENERATED ALWAYS AS (df(id)) STORED);
	INSERT INTO dropped VALUES (1, 1, 'z', ROW('z'))});
caught_up();
run('DROP DOMAIN dd CASCADE');
same_map('dropped', 'a domain dropped, and a column with it');
run('DROP FUNCTION df(integer) CASCADE');
same_map('dropped', 'a function dropped, and a generated column with it');
run('DROP TYPE dt CASCADE');
same_map('dropped', 'a type dropped, and a column and an attribute with it');

# a key changed in two statements of one transaction, or by a partitioned table's
run('CREATE TABLE swap (id integer PRIMARY KEY, a integer NOT NULL); INSERT INTO swap VALUES (1, 2)');
caught_up();
run('BEGIN; ALTER TABLE swap DROP CONSTRAINT swap_pkey; INSERT INTO swap VALUES (2, 3); ALTER TABLE swap ADD PRIMARY KEY (a); COMMIT');
same_map('swap', 'a key swapped in two statements, with a row between');
run('CREATE TABLE pk (id integer, a integer NOT NULL, PRIMARY KEY (id, a)) PARTITION BY RANGE (id);
	CREATE TABLE pk1 PARTITION OF pk FOR VALUES FROM (0) TO (10); INSERT INTO pk VALUES (1, 1)');
caught_up();
run('ALTER TABLE pk DROP CONSTRAINT pk_pkey; ALTER TABLE pk ADD PRIMARY KEY (a, id)');
same_map('pk1', 'a partitioned table\'s key changed');

# a table's file changing, with its rows the same
run('CREATE TABLE same_file (id integer PRIMARY KEY, a integer); INSERT INTO same_file VALUES (1, 1)');
caught_up();
$node->safe_psql('postgres', "$on1 VACUUM FULL s.same_file");
same_map('same_file', 'VACUUM FULL');
$node->safe_psql('postgres', "$on1 TRUNCATE s.same_file");
same_map('same_file', 'a truncate');

# a type's owner, renaming a value of it, needn't reach the tables using it
$node->safe_psql('postgres', q{CREATE ROLE own_e LOGIN; SET ROLE own_e; RESET ROLE; CREATE TYPE public.oe AS ENUM ('o'); ALTER TYPE public.oe OWNER TO own_e});
run(q{CREATE TABLE owned_e (id integer PRIMARY KEY, a public.oe); INSERT INTO owned_e VALUES (1, 'o')});
caught_up();
$node->safe_psql('postgres', q{SET ROLE own_e; ALTER TYPE public.oe RENAME VALUE 'o' TO 'oo'});
same_map('owned_e', 'a type\'s owner, with no right to the table');

# nobody else can write what only a statement's commit may: not with any rights
$node->safe_psql('postgres', 'CREATE ROLE forger LOGIN');
($rc, $out, $err) = $node->psql('postgres', q{SET ROLE forger; SELECT pg_logical_emit_message(true, 'prolly.capture.rows', 'R')});
like($err, qr/prefix may not start with "prolly\."/, 'a role cannot forge one');
($rc, $out, $err) = $node->psql('postgres', q{SELECT pg_logical_emit_message(true, 'prolly.capture.rows', 'R')});
like($err, qr/prefix may not start with "prolly\."/, 'nor a superuser');

# two in the commit at once, the first having read and written and not yet committed: the second
# waits, and reads what the first did
SKIP:
{
	skip 'injection points not supported by this build', 2 if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	$node->safe_psql('postgres', q{CREATE TYPE public.e4 AS ENUM ('d'); CREATE TYPE public.e5 AS ENUM ('f')});
	run(q{CREATE TABLE overlap (id integer PRIMARY KEY, x public.e4, y public.e5); INSERT INTO overlap VALUES (1, 'd', 'f')});
	caught_up();
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-rows-written', 'wait')});
	my $first = $node->background_psql('postgres');
	my $second = $node->background_psql('postgres');
	$first->query_safe(q{BEGIN; ALTER TYPE public.e4 RENAME VALUE 'd' TO 'd4'});
	$second->query_safe(q{BEGIN; ALTER TYPE public.e5 RENAME VALUE 'f' TO 'f5'});
	$first->query_until(qr/started/, "\\echo started\nCOMMIT;\n\\echo finished\n");
	$node->wait_for_event('client backend', 'capture-rows-written');
	$second->query_until(qr/started/, "\\echo started\nCOMMIT;\n\\echo finished\n");
	ok( $node->poll_query_until('postgres',
			q{SELECT count(*) = 1 FROM pg_stat_activity WHERE query = 'COMMIT;' AND wait_event_type = 'Lock' AND wait_event = 'object'}),
		'the second waits its turn');
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-rows-written')});
	$node->safe_psql('postgres', q{SELECT injection_points_wakeup('capture-rows-written')});
	$first->quit;
	$second->quit;
	same_map('overlap', 'and has what the first did');

	# and with neither changing a table's fingerprint yet: the second's new table has the first's
	$node->safe_psql('postgres', q{CREATE TYPE public.e6 AS ENUM ('g')});
	my $maker = $node->background_psql('postgres');
	$maker->query_safe("$on1 BEGIN; CREATE TABLE s.made_meanwhile (id integer PRIMARY KEY, a public.e6); INSERT INTO s.made_meanwhile VALUES (1, 'g')");
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-rows-written', 'wait')});
	my $renamer = $node->background_psql('postgres');
	$renamer->query_safe(q{BEGIN; ALTER TYPE public.e6 RENAME VALUE 'g' TO 'g6'});
	$renamer->query_until(qr/started/, "\\echo started\nCOMMIT;\n\\echo finished\n");
	$node->wait_for_event('client backend', 'capture-rows-written');
	$maker->query_until(qr/started/, "\\echo started\nCOMMIT;\n\\echo finished\n");
	$node->poll_query_until('postgres',
		q{SELECT count(*) = 1 FROM pg_stat_activity WHERE query = 'COMMIT;' AND wait_event_type = 'Lock' AND wait_event = 'object'})
	  or die 'the second did not wait';
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-rows-written')});
	$node->safe_psql('postgres', q{SELECT injection_points_wakeup('capture-rows-written')});
	$renamer->quit;
	$maker->quit;
	same_map('made_meanwhile', 'a table committed while a rename that changed no table was committing');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
}

# a change told again, in a layout before DDL that's told again too, doesn't stop it
SKIP:
{
	skip 'injection points not supported by this build', 1 if $ENV{enable_injection_points} ne 'yes' || $windows_os;
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	run('CREATE TABLE retold (id integer PRIMARY KEY, a integer); INSERT INTO retold VALUES (1, 1)');
	caught_up();
	my $worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-applied', 'wait')});
	kill 'STOP', $worker;
	run('INSERT INTO retold VALUES (2, 2)');
	run('ALTER TABLE retold ADD COLUMN b integer DEFAULT 9');
	kill 'CONT', $worker;
	$node->wait_for_event('capture worker', 'capture-applied');
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applied')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($worker)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $worker})
	  or die 'no worker came back';
	run('INSERT INTO retold VALUES (3, 3, 3)');
	same_map('retold', 'told again across a layout changing');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
}

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
