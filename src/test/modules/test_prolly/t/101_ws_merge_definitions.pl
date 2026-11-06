# Copyright (c) 2026, PostgreSQL Global Development Group

# What a merge works values out by: its merged definitions, whichever side they came from,
# or a conflict.  Each scenario's schema has one name on three branches, as a real one does.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('definitions');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
$node->safe_psql('postgres', 'CREATE ROLE g_def NOLOGIN TRAVELS');
my $branch = sub { $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()') };

# A merge of app on three branches; its conflicts, and a table's rows of what it made.
sub scenario
{
	my ($base, $ours, $theirs, $then) = @_;
	my ($b, $o, $t) = ($branch->(), $branch->(), $branch->());
	my $side = sub {
		my ($on, $sql) = @_;
		return qq{SET catalog_branch = $on; CREATE SCHEMA app VERSIONED; ALTER SCHEMA app OWNER TO g_def; SET ROLE g_def; SET search_path = app; $base; $sql; RESET ROLE; SET search_path = public;\n};
	};
	my $script = "SET client_min_messages = warning;\n" . $side->($b, '') . qq{SELECT test_ws_root_capture('app') AS rb \\gset\n}
	  . $side->($o, $ours) . qq{SELECT test_ws_root_capture('app') AS ro \\gset\n}
	  . $side->($t, $theirs) . qq{SELECT test_ws_root_capture('app') AS rt \\gset\n}
	  . ($then ? join('', map { "SET catalog_branch = $_; DROP SCHEMA app CASCADE;\n" } ($b, $o, $t)) . $then : '')
	  . qq{SET catalog_branch = $o;\n} . qq{
	SELECT test_ws_merge(:'rb', :'ro', :'rt', 'app') AS merged \\gset
	\\echo =====merge
	\\echo :merged
	\\echo =====end
	SELECT split_part(:'merged', E'\\n', 1) AS m \\gset
	SELECT count(*) AS left FROM pg_namespace WHERE nspname = 'app' \\gset
	\\echo =====left
	\\echo :left
	\\echo =====end
	};
	my $d = $branch->();
	$script .= qq{SET catalog_branch = $d; CREATE SCHEMA app VERSIONED; ALTER SCHEMA app OWNER TO g_def;
	SELECT test_ws_root_restore(:'m', 'app') IS NULL AS r \\gset
	\\echo =====rows
	SELECT * FROM app.t ORDER BY 1;
	\\echo =====end
	};
	my ($rc, $out, $err) = $node->psql('postgres', $script);
	my ($merge) = $out =~ /=====merge\n(.*?)\n?=====end/s;
	my @conflicts = split /\n/, $merge // '';
	shift @conflicts;
	my ($rows) = $out =~ /=====rows\n(.*?)\n?=====end/s;
	return (\@conflicts, $rows // '', $err);
}

my $T = q{CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer)};

# a function's text naming its own schema: the merged definition, from either side
my $HELPER = q{CREATE FUNCTION helper(integer) RETURNS integer IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN $1 * 10; END $$;
	CREATE FUNCTION q(integer) RETURNS integer IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN app.helper($1); END $$;
	CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (q(a)) STORED);
	INSERT INTO t VALUES (1, 1, 1)};
my $REPLACE = q{CREATE OR REPLACE FUNCTION helper(integer) RETURNS integer IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN $1 * 100; END $$};
my ($c, $r, $e) = scenario($HELPER, q{UPDATE t SET a = 9}, "$REPLACE; UPDATE t SET b = 5");
is_deeply($c, [], 'a body naming its schema, replaced by theirs: clean') or diag($e);
is($r, '1|9|5|900', 'and by the merged definition');
($c, $r, $e) = scenario($HELPER, "$REPLACE; UPDATE t SET a = 9", q{UPDATE t SET b = 5});
is($r, '1|9|5|900', 'replaced by ours, the same');

# a function reading a table has no rows to read: a conflict, not a value
my $READS = q{CREATE TABLE cfg (k integer PRIMARY KEY, v integer); INSERT INTO cfg VALUES (1, 100);
	CREATE FUNCTION lk(integer) RETURNS integer IMMUTABLE LANGUAGE plpgsql AS $$ DECLARE r integer; BEGIN SELECT v INTO r FROM cfg WHERE k = 1; RETURN r + $1; END $$};
($c, $r, $e) = scenario("$READS; CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (lk(a)) STORED); INSERT INTO t VALUES (1, 1, 1)",
	q{UPDATE t SET a = 5}, q{UPDATE t SET b = 5});
is_deeply($c, [ 'generated t 1' ], 'a generated value reading a table conflicts') or diag($e);
($c, $r, $e) = scenario("$READS; $T", q{ALTER TABLE t ADD COLUMN d integer DEFAULT lk(0)}, q{INSERT INTO t VALUES (1, 1, 1)});
is_deeply($c, [ 'generated t 1' ], 'a default reading a table conflicts') or diag($e);

# what is made after tables, which a function's text may use all the same
($c, $r, $e) = scenario(q{CREATE TABLE cfg (k integer PRIMARY KEY);
	CREATE FUNCTION after_fn(c cfg) RETURNS integer IMMUTABLE LANGUAGE sql AS 'SELECT 2';
	CREATE FUNCTION f(integer) RETURNS integer IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN after_fn(ROW(1)::cfg) + $1; END $$;
	CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (f(a)) STORED);
	INSERT INTO t VALUES (1, 1, 1)}, q{UPDATE t SET a = 10}, q{UPDATE t SET b = 5});
is_deeply($c, [], 'a body using what is made after tables: clean') or diag($e);
is($r, '1|10|5|12', 'and its value');

# what PUBLIC was refused stays callable by what calls it
($c, $r, $e) = scenario(q{CREATE FUNCTION inner_f(integer) RETURNS integer IMMUTABLE LANGUAGE sql AS 'SELECT $1 * 2';
	REVOKE ALL ON FUNCTION inner_f(integer) FROM PUBLIC;
	CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (inner_f(a)) STORED);
	INSERT INTO t VALUES (1, 1, 1)}, q{UPDATE t SET a = 10}, q{UPDATE t SET b = 5});
is_deeply($c, [], 'a function PUBLIC cannot call: clean') or diag($e);
is($r, '1|10|5|20', 'and its value');

# a function that runs as an owner not here is no value
$node->safe_psql('postgres', 'CREATE ROLE g_gone NOLOGIN TRAVELS; GRANT g_gone TO g_def');
($c, $r, $e) = scenario(q{CREATE FUNCTION who(integer) RETURNS text IMMUTABLE SECURITY DEFINER LANGUAGE sql AS 'SELECT current_user::text';
	RESET ROLE; GRANT CREATE ON SCHEMA app TO g_gone; SET ROLE g_def;
	ALTER FUNCTION who(integer) OWNER TO g_gone;
	CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g text GENERATED ALWAYS AS (who(a)) STORED);
	INSERT INTO t VALUES (1, 1, 1)}, q{UPDATE t SET a = 10}, q{UPDATE t SET b = 5},
	q{DROP ROLE g_gone;});
is(scalar @$c, 1, 'a definer whose owner is gone: one conflict') or diag($e);
like($c->[0] // '', qr/^unapplied function "who" runs as its owner/, 'it says so');

# the message outlasts the row that first asked for the definitions
$node->safe_psql('postgres', 'CREATE ROLE g_gone2 NOLOGIN TRAVELS; GRANT g_gone2 TO g_def');
($c, $r, $e) = scenario(q{CREATE FUNCTION who(integer) RETURNS text IMMUTABLE SECURITY DEFINER LANGUAGE sql AS 'SELECT current_user::text';
	RESET ROLE; GRANT CREATE ON SCHEMA app TO g_gone2; SET ROLE g_def;
	ALTER FUNCTION who(integer) OWNER TO g_gone2;
	CREATE TABLE t (id integer PRIMARY KEY, a integer, n text)},
	q{ALTER TABLE t ALTER COLUMN a TYPE bigint}, q{INSERT INTO t SELECT g, g, repeat('x', 100) FROM generate_series(1, 3000) g},
	q{DROP ROLE g_gone2;});
is(scalar @$c, 1, 'asked for within a row: one conflict') or diag($e);
like($c->[0] // '', qr/^unapplied function "who" runs as its owner/, 'its message intact');

# someone else's owner being gone is nothing to a definer the merger owns
$node->safe_psql('postgres', 'CREATE ROLE g_gone3 NOLOGIN TRAVELS; GRANT g_gone3 TO g_def');
($c, $r, $e) = scenario(q{CREATE TYPE ty AS ENUM ('a');
	RESET ROLE; GRANT CREATE ON SCHEMA app TO g_gone3; SET ROLE g_def;
	ALTER TYPE ty OWNER TO g_gone3;
	CREATE FUNCTION mine(integer) RETURNS integer IMMUTABLE SECURITY DEFINER LANGUAGE sql AS 'SELECT $1 * 2';
	CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (mine(a)) STORED);
	INSERT INTO t VALUES (1, 1, 1)}, q{UPDATE t SET a = 10}, q{UPDATE t SET b = 5},
	q{DROP ROLE g_gone3;});
is_deeply($c, [], 'a definer of its own, another object with its owner gone: clean') or diag($e);

# a procedure that runs as an owner not here, as a function does
$node->safe_psql('postgres', 'CREATE ROLE g_gone4 NOLOGIN TRAVELS; GRANT g_gone4 TO g_def');
($c, $r, $e) = scenario(q{CREATE PROCEDURE who(INOUT x text) SECURITY DEFINER LANGUAGE plpgsql AS $$ BEGIN x := current_user; END $$;
	RESET ROLE; GRANT CREATE ON SCHEMA app TO g_gone4; SET ROLE g_def;
	ALTER PROCEDURE who(text) OWNER TO g_gone4;
	CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (a * 2) STORED);
	INSERT INTO t VALUES (1, 1, 1)}, q{UPDATE t SET a = 10}, q{UPDATE t SET b = 5},
	q{DROP ROLE g_gone4;});
like($c->[0] // '', qr/^unapplied procedure "who" runs as its owner/, 'a definer procedure with its owner gone') or diag($e);

# another of its name is another function
$node->safe_psql('postgres', 'CREATE ROLE g_gone5 NOLOGIN TRAVELS; GRANT g_gone5 TO g_def');
($c, $r, $e) = scenario(q{CREATE FUNCTION f(integer) RETURNS integer IMMUTABLE LANGUAGE sql AS 'SELECT $1';
	RESET ROLE; GRANT CREATE ON SCHEMA app TO g_gone5; SET ROLE g_def;
	ALTER FUNCTION f(integer) OWNER TO g_gone5;
	CREATE FUNCTION f(text) RETURNS integer IMMUTABLE SECURITY DEFINER LANGUAGE sql AS 'SELECT 1';
	CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (a * 2) STORED);
	INSERT INTO t VALUES (1, 1, 1)}, q{UPDATE t SET a = 10}, q{UPDATE t SET b = 5},
	q{DROP ROLE g_gone5;});
is_deeply($c, [], 'a definer of its own beside another of its name, whose owner is gone: clean') or diag($e);

# a body that names a constraint has it there
($c, $r, $e) = scenario(q{CREATE TABLE p (id integer PRIMARY KEY, u integer, CONSTRAINT pk UNIQUE (u));
	CREATE FUNCTION put(integer) RETURNS void LANGUAGE sql BEGIN ATOMIC INSERT INTO p VALUES ($1, $1) ON CONFLICT ON CONSTRAINT pk DO NOTHING; END;
	CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (a * 2) STORED);
	INSERT INTO t VALUES (1, 1, 1)}, q{UPDATE t SET a = 10}, q{UPDATE t SET b = 5});
is_deeply($c, [], 'a body naming a constraint: clean') or diag($e);
is($r, '1|10|5|20', 'and its value');

# a sequence hands out nothing it held
($c, $r, $e) = scenario(q{CREATE SEQUENCE s CYCLE;
	CREATE FUNCTION nv(integer) RETURNS bigint IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN nextval('app.s'); END $$;
	CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g bigint GENERATED ALWAYS AS (nv(a)) STORED);
	INSERT INTO t VALUES (1, 1, 1)}, q{UPDATE t SET a = 10}, q{UPDATE t SET b = 5});
is_deeply($c, [ 'generated t 1' ], 'a value from a sequence conflicts') or diag($e);

# a schema conflict hides what its rows would say
($c, $r, $e) = scenario("$T; INSERT INTO t VALUES (1, 1, 1); CREATE TABLE u (id integer PRIMARY KEY, a integer); INSERT INTO u VALUES (1, 1)",
	q{ALTER TABLE t ALTER COLUMN a TYPE bigint; UPDATE u SET a = 2}, q{ALTER TABLE t ALTER COLUMN a TYPE numeric; UPDATE u SET a = 3});
is_deeply($c, [ 'column t.a' ], 'a schema conflict alone, no row of another table');

# a merge with nothing to work out makes nothing, so needs no branch or write
my $nothing = $node->safe_psql('postgres', q{SET client_min_messages = warning;
	SET catalog_branch = } . $branch->() . q{; CREATE SCHEMA n1 VERSIONED; CREATE TABLE n1.t (id integer PRIMARY KEY); INSERT INTO n1.t VALUES (1);
	SELECT test_ws_root_capture('n1') AS r \gset
	BEGIN READ ONLY;
	SELECT split_part(test_ws_merge(:'r', :'r', :'r'), E'\n', 2);
	COMMIT;});
is($nothing, '', 'read only, nothing to work out: clean');

# a merge within a value being worked out comes back
my $NEST = q{CREATE FUNCTION nest(integer) RETURNS integer IMMUTABLE LANGUAGE plpgsql AS $$
	DECLARE b text := current_setting('my.ib', true);
	BEGIN
		IF b IS NOT NULL AND b <> '' THEN
			PERFORM set_config('my.ib', '', false);
			PERFORM public.test_ws_merge(b, current_setting('my.io'), current_setting('my.it'), 'inner');
		END IF;
		RETURN $1;
	END $$;
	CREATE TABLE t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (nest(a)) STORED);
	INSERT INTO t VALUES (1, 1, 1)};
my ($nest_rc, $nest, $nest_err) = $node->psql('postgres', qq{SET client_min_messages = warning;
	SET catalog_branch = } . $branch->() . qq{;
	CREATE SCHEMA nb VERSIONED; CREATE SCHEMA no VERSIONED; CREATE SCHEMA nt VERSIONED;
	SET search_path = nb; $NEST;
	SET search_path = no; $NEST; UPDATE t SET a = 9;
	SET search_path = nt; $NEST; UPDATE t SET b = 5;
	SET search_path = public;
	SELECT test_ws_root_capture('nb') AS rb \\gset
	SELECT test_ws_root_capture('no') AS ro \\gset
	SELECT test_ws_root_capture('nt') AS rt \\gset
	SELECT set_config('my.ib', :'rb', false), set_config('my.io', :'ro', false), set_config('my.it', :'rt', false) \\gset
	SELECT split_part(test_ws_merge(:'rb', :'ro', :'rt', 'outer'), E'\\n', 2);
	SELECT 1 FROM pg_class LIMIT 1;}, timeout => 60);
is($nest_rc, 0, 'a merge within a value: comes back') or diag($nest_err);

# a value merged by key is what capture writes, whatever the database's encoding
$node->safe_psql('postgres', q{CREATE DATABASE latin TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C'});
$node->safe_psql('latin', 'CREATE EXTENSION test_prolly');
{
	my $on = $node->safe_psql('latin', 'SELECT pg_branch_id_allocate()');
	my ($l_rc, $l_out, $l_err) = $node->psql('latin', qq{SET client_min_messages = warning;
		SET client_encoding = 'UTF8';
		SET catalog_branch = $on;
		CREATE SCHEMA lb VERSIONED; CREATE SCHEMA lo VERSIONED; CREATE SCHEMA lt VERSIONED; CREATE SCHEMA ld VERSIONED;
		CREATE TABLE lb.t (id integer PRIMARY KEY, j jsonb); INSERT INTO lb.t VALUES (1, '{"é": 1, "ab": 2, "c": 3}');
		CREATE TABLE lo.t (id integer PRIMARY KEY, j jsonb); INSERT INTO lo.t VALUES (1, '{"é": 1, "ab": 2, "c": 4}');
		CREATE TABLE lt.t (id integer PRIMARY KEY, j jsonb); INSERT INTO lt.t VALUES (1, '{"é": 1, "ab": 2, "c": 3, "zz": 5}');
		SELECT split_part(test_ws_merge(test_ws_root_capture('lb'), test_ws_root_capture('lo'), test_ws_root_capture('lt'), 'ld'), E'\n', 1) AS m \\gset
		SELECT test_ws_root_restore(:'m', 'ld') IS NULL AS r \\gset
		SELECT test_ws_root_capture('ld') = :'m', (SELECT j::text FROM ld.t);});
	is($l_rc, 0, 'a merge by key in LATIN1: runs') or diag($l_err);
	like($l_out, qr/^t\|\{.*"é": 1.*"zz": 5\}$/m, 'and it captures as the root it made');
}

# two merges in one transaction, as a role with no more rights than its schemas'
my ($two_rc, $two, $two_err) = $node->psql('postgres', q{SET client_min_messages = warning;
	CREATE ROLE merger LOGIN; GRANT g_def TO merger;
	SET catalog_branch = } . $branch->() . q{; CREATE SCHEMA m1 VERSIONED; ALTER SCHEMA m1 OWNER TO g_def;
	CREATE SCHEMA m2 VERSIONED; ALTER SCHEMA m2 OWNER TO g_def;
	SET ROLE g_def;
	CREATE TABLE m1.t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (a * 2) STORED); INSERT INTO m1.t VALUES (1, 1, 1);
	CREATE TABLE m2.t (id integer PRIMARY KEY, a integer, b integer, g integer GENERATED ALWAYS AS (a * 2) STORED); INSERT INTO m2.t VALUES (1, 1, 1);
	RESET ROLE;
	SELECT test_ws_root_capture('m1') AS rb \gset
	UPDATE m1.t SET a = 5; SELECT test_ws_root_capture('m1') AS ro \gset
	UPDATE m2.t SET b = 5; SELECT test_ws_root_capture('m2') AS rt \gset
	SET ROLE merger;
	BEGIN;
	SELECT split_part(test_ws_merge(:'rb', :'ro', :'rt', 'x1'), E'\n', 2);
	SELECT split_part(test_ws_merge(:'rb', :'ro', :'rt', 'x1'), E'\n', 2);
	COMMIT;
	RESET ROLE;
	SELECT count(*) FROM pg_namespace WHERE nspname = 'x1';});
is($two_rc, 0, 'two merges in a transaction, by a role with no more rights') or diag($two_err);
like($two, qr/^\s*\n?\s*\n?0$/, 'clean both times, and nothing left');

$node->stop;
done_testing();
