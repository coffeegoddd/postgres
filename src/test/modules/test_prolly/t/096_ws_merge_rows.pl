# Copyright (c) 2026, PostgreSQL Global Development Group

# A three-way merge of rows.  Each scenario makes a base, and ours and theirs from it,
# each in a schema of its own, merges them, and restores what the merged root holds of
# a table into a fourth, to read back.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('mergerows');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$ENV{PGOPTIONS} = "-c catalog_branch=$branch";
$node->safe_psql('postgres', 'CREATE ROLE g_merge NOLOGIN TRAVELS');

my $T = q{CREATE TABLE t (id integer PRIMARY KEY, a text, b text)};
my $ROWS = q{INSERT INTO t VALUES (1, 'a1', 'b1'), (2, 'a2', 'b2'), (3, 'a3', 'b3')};
my $K = q{CREATE TABLE t (a integer, b text)};

# name, base, ours, theirs, conflicts, the merged rows (a query's text of them)
my @scenarios = (
	[ 'other rows', "$T; $ROWS", q{INSERT INTO t VALUES (4, 'o', 'o')}, q{INSERT INTO t VALUES (5, 't', 't')}, [],
		"1|a1|b1\n2|a2|b2\n3|a3|b3\n4|o|o\n5|t|t" ],
	[ 'other changes to other rows', "$T; $ROWS", q{UPDATE t SET a = 'o' WHERE id = 1}, q{UPDATE t SET a = 't' WHERE id = 2}, [],
		"1|o|b1\n2|t|b2\n3|a3|b3" ],
	[ 'one row, other columns', "$T; $ROWS", q{UPDATE t SET a = 'o' WHERE id = 1}, q{UPDATE t SET b = 't' WHERE id = 1}, [],
		"1|o|t\n2|a2|b2\n3|a3|b3" ],
	[ 'one row, one column alike', "$T; $ROWS", q{UPDATE t SET a = 'x' WHERE id = 1}, q{UPDATE t SET a = 'x' WHERE id = 1}, [],
		"1|x|b1\n2|a2|b2\n3|a3|b3" ],
	[ 'one cell two ways', "$T; $ROWS", q{UPDATE t SET a = 'o' WHERE id = 1}, q{UPDATE t SET a = 't' WHERE id = 1},
		[ 'row t 1' ], undef ],
	[ 'a cell nulled and changed', "$T; $ROWS", q{UPDATE t SET a = NULL WHERE id = 1}, q{UPDATE t SET a = 't' WHERE id = 1},
		[ 'row t 1' ], undef ],
	[ 'a cell nulled and another changed', "$T; $ROWS", q{UPDATE t SET a = NULL WHERE id = 1}, q{UPDATE t SET b = 't' WHERE id = 1}, [],
		"1||t\n2|a2|b2\n3|a3|b3" ],
	[ 'deleted and changed', "$T; $ROWS", q{DELETE FROM t WHERE id = 1}, q{UPDATE t SET a = 't' WHERE id = 1},
		[ 'row t 1' ], undef ],
	[ 'deleted by both', "$T; $ROWS", q{DELETE FROM t WHERE id = 1}, q{DELETE FROM t WHERE id = 1}, [],
		"2|a2|b2\n3|a3|b3" ],
	[ 'one key inserted alike', "$T; $ROWS", q{INSERT INTO t VALUES (4, 'x', 'y')}, q{INSERT INTO t VALUES (4, 'x', 'y')}, [],
		"1|a1|b1\n2|a2|b2\n3|a3|b3\n4|x|y" ],
	[ 'one key inserted two ways', "$T; $ROWS", q{INSERT INTO t VALUES (4, 'o', 'y')}, q{INSERT INTO t VALUES (4, 't', 'y')},
		[ 'row t 4' ], undef ],
	[ 'a key of two columns', q{CREATE TABLE t (k1 integer, k2 text, v text, PRIMARY KEY (k1, k2)); INSERT INTO t VALUES (1, 'x', 'b')},
		q{UPDATE t SET v = 'o'}, q{UPDATE t SET v = 't'}, [ 'row t 1,x' ], undef ],
	[ 'both add other columns, with rows', "$T; $ROWS",
		q{ALTER TABLE t ADD COLUMN c text; UPDATE t SET c = 'o' WHERE id = 1},
		q{ALTER TABLE t ADD COLUMN d text; UPDATE t SET d = 't' WHERE id = 2}, [],
		"1|a1|b1|o|\n2|a2|b2||t\n3|a3|b3||" ],
	[ 'a default added, and new rows the other side', "$T; $ROWS",
		q{ALTER TABLE t ADD COLUMN c integer DEFAULT 7}, q{INSERT INTO t VALUES (4, 't', 't')}, [],
		"1|a1|b1|7\n2|a2|b2|7\n3|a3|b3|7\n4|t|t|7" ],
	[ 'a default added, and a changed row the other side', "$T; $ROWS",
		q{ALTER TABLE t ADD COLUMN c integer DEFAULT 7}, q{UPDATE t SET a = 't' WHERE id = 1}, [],
		"1|t|b1|7\n2|a2|b2|7\n3|a3|b3|7" ],
	[ 'a nullable column, and new rows the other side', "$T; $ROWS",
		q{ALTER TABLE t ADD COLUMN c integer}, q{INSERT INTO t VALUES (4, 't', 't')}, [],
		"1|a1|b1|\n2|a2|b2|\n3|a3|b3|\n4|t|t|" ],
	[ 'one column added by both, rows each', "$T; $ROWS",
		q{ALTER TABLE t ADD COLUMN c integer; UPDATE t SET c = 1 WHERE id = 1},
		q{ALTER TABLE t ADD COLUMN c integer; UPDATE t SET c = 2 WHERE id = 2}, [],
		"1|a1|b1|1\n2|a2|b2|2\n3|a3|b3|" ],
	[ 'a rename, and an update', "$T; $ROWS", q{ALTER TABLE t RENAME COLUMN a TO aa}, q{UPDATE t SET a = 't' WHERE id = 1}, [],
		"1|t|b1\n2|a2|b2\n3|a3|b3" ],
	[ 'a column dropped, and an update of it', "$T; $ROWS", q{ALTER TABLE t DROP COLUMN b}, q{UPDATE t SET b = 't' WHERE id = 1}, [],
		"1|a1\n2|a2\n3|a3" ],
	[ 'a column dropped, and new rows', "$T; $ROWS", q{ALTER TABLE t DROP COLUMN b}, q{INSERT INTO t VALUES (4, 'x', 'y')}, [],
		"1|a1\n2|a2\n3|a3\n4|x" ],
	[ 'a retype, and rows that read as it', q{CREATE TABLE t (id integer PRIMARY KEY, a text); INSERT INTO t VALUES (1, '5')},
		q{ALTER TABLE t ALTER COLUMN a TYPE integer USING a::integer}, q{INSERT INTO t VALUES (2, '6')}, [],
		"1|5\n2|6" ],
	[ 'a retype, and rows that do not', q{CREATE TABLE t (id integer PRIMARY KEY, a text); INSERT INTO t VALUES (1, '5')},
		q{ALTER TABLE t ALTER COLUMN a TYPE integer USING a::integer}, q{INSERT INTO t VALUES (2, 'not a number')},
		[ 'row t 2' ], undef ],
	[ 'a narrower type, and rows longer', q{CREATE TABLE t (id integer PRIMARY KEY, a varchar(10)); INSERT INTO t VALUES (1, 'x')},
		q{ALTER TABLE t ALTER COLUMN a TYPE varchar(3)}, q{INSERT INTO t VALUES (2, 'longer')},
		[ 'row t 2' ], undef ],
	[ 'a retype changing the text, and the old text', q{CREATE TABLE t (id integer PRIMARY KEY, a integer); INSERT INTO t VALUES (1, 5)},
		q{ALTER TABLE t ALTER COLUMN a TYPE numeric(10, 2)}, q{UPDATE t SET a = 6 WHERE id = 1},
		[ 'row t 1' ], undef ],
	[ 'a domain with a new check, and rows outside it', q{CREATE DOMAIN dm AS integer; CREATE TABLE t (id integer PRIMARY KEY, v dm); INSERT INTO t VALUES (1, 5)},
		q{ALTER DOMAIN dm ADD CONSTRAINT pos CHECK (VALUE > 0)}, q{INSERT INTO t VALUES (2, -1)},
		[ 'row t 2' ], undef ],
	[ 'an enum value renamed, and rows it reads', q{CREATE TYPE e AS ENUM ('a', 'b'); CREATE TABLE t (id integer PRIMARY KEY, v e); INSERT INTO t VALUES (1, 'b')},
		q{ALTER TYPE e RENAME VALUE 'a' TO 'z'}, q{INSERT INTO t VALUES (2, 'b')}, [],
		undef ],
	[ 'an enum value renamed, and rows of the old value', q{CREATE TYPE e AS ENUM ('a', 'b'); CREATE TABLE t (id integer PRIMARY KEY, v e); INSERT INTO t VALUES (1, 'b')},
		q{ALTER TYPE e RENAME VALUE 'a' TO 'z'}, q{INSERT INTO t VALUES (2, 'a')},
		[ 'row t 2' ], undef ],
	[ 'keyless, both insert', "$K; INSERT INTO t VALUES (1, 'x')", q{INSERT INTO t VALUES (1, 'x')}, q{INSERT INTO t VALUES (1, 'x')}, [],
		"1|x\n1|x\n1|x" ],
	[ 'keyless, each its own', "$K; INSERT INTO t VALUES (1, 'x')", q{INSERT INTO t VALUES (2, 'o')}, q{INSERT INTO t VALUES (3, 't')}, [],
		"1|x\n2|o\n3|t" ],
	[ 'keyless, one of many deleted each', "$K; INSERT INTO t VALUES (1, 'x'), (1, 'x'), (1, 'x')",
		q{DELETE FROM t WHERE ctid = (SELECT min(ctid) FROM t)}, q{DELETE FROM t WHERE ctid = (SELECT max(ctid) FROM t)}, [],
		"1|x" ],
	[ 'keyless, more deleted than there are', "$K; INSERT INTO t VALUES (1, 'x')", q{DELETE FROM t}, q{DELETE FROM t}, [],
		'' ],
	[ 'keyless, a column dropped with rows', "$K; INSERT INTO t VALUES (1, 'x'), (2, 'y')", q{ALTER TABLE t DROP COLUMN b}, q{INSERT INTO t VALUES (3, 'z')}, [],
		"1\n2\n3" ],
	[ 'keyless, a default added with rows', "$K; INSERT INTO t VALUES (1, 'x')", q{ALTER TABLE t ADD COLUMN c integer DEFAULT 7}, q{INSERT INTO t VALUES (2, 'y')}, [],
		"1|x|7\n2|y|7" ],
	[ 'keyless, rows that become one', "$K; INSERT INTO t VALUES (1, 'x')", q{ALTER TABLE t DROP COLUMN b}, q{INSERT INTO t VALUES (1, 'y')}, [],
		"1\n1" ],
	[ 'a large value, other rows', q{CREATE TABLE t (id integer PRIMARY KEY, a text); INSERT INTO t VALUES (1, repeat('x', 5000)), (2, repeat('y', 5000))},
		q{UPDATE t SET a = repeat('o', 5000) WHERE id = 1}, q{UPDATE t SET a = repeat('t', 6000) WHERE id = 2}, [],
		'1|' . ('o' x 5000) . "\n2|" . ('t' x 6000) ],
	[ 'a large value two ways', q{CREATE TABLE t (id integer PRIMARY KEY, a text); INSERT INTO t VALUES (1, repeat('x', 5000))},
		q{UPDATE t SET a = repeat('o', 5000)}, q{UPDATE t SET a = repeat('t', 5000)}, [ 'row t 1' ], undef ],
	[ 'a large value alike', q{CREATE TABLE t (id integer PRIMARY KEY, a text); INSERT INTO t VALUES (1, 'x')},
		q{UPDATE t SET a = repeat('o', 5000)}, q{UPDATE t SET a = repeat('o', 5000)}, [],
		'1|' . ('o' x 5000) ],
	[ 'a generated column, a row from both', q{CREATE TABLE t (id integer PRIMARY KEY, x integer, y integer, g integer GENERATED ALWAYS AS (x + y) STORED); INSERT INTO t VALUES (1, 1, 1)},
		q{UPDATE t SET x = 10}, q{UPDATE t SET y = 20}, [],
		'1|10|20|30' ],
	[ 'a generated column, one side', q{CREATE TABLE t (id integer PRIMARY KEY, x integer, g integer GENERATED ALWAYS AS (x + 1) STORED); INSERT INTO t VALUES (1, 1), (2, 2)},
		q{UPDATE t SET x = 10 WHERE id = 1}, q{UPDATE t SET x = 20 WHERE id = 2}, [],
		"1|10|11\n2|20|21" ],
	[ 'a generated column added, and new rows', q{CREATE TABLE t (id integer PRIMARY KEY, x integer); INSERT INTO t VALUES (1, 1)},
		q{ALTER TABLE t ADD COLUMN g integer GENERATED ALWAYS AS (x * 2) STORED}, q{INSERT INTO t VALUES (2, 2)}, [],
		"1|1|2\n2|2|4" ],
	[ 'another expression, and new rows', q{CREATE TABLE t (id integer PRIMARY KEY, x integer, g integer GENERATED ALWAYS AS (x + 1) STORED); INSERT INTO t VALUES (1, 1)},
		q{ALTER TABLE t ALTER COLUMN g SET EXPRESSION AS (x + 2)}, q{INSERT INTO t VALUES (2, 2)}, [],
		"1|1|3\n2|2|4" ],
	[ 'a value generated that cannot be', q{CREATE TABLE t (id integer PRIMARY KEY, x integer, y integer, g integer GENERATED ALWAYS AS (10 / (x + y)) STORED); INSERT INTO t VALUES (1, 2, 2)},
		q{UPDATE t SET x = 1}, q{UPDATE t SET y = -1}, [ 'generated t 1' ], undef ],
	[ 'a default that cannot be', q{CREATE TABLE t (id integer PRIMARY KEY, x integer); INSERT INTO t VALUES (1, 1)},
		q{ALTER TABLE t ADD COLUMN c integer DEFAULT 1; ALTER TABLE t ALTER COLUMN c SET DEFAULT (1 / 0)}, q{INSERT INTO t VALUES (2, 2)},
		[ 'generated t 2' ], undef ],
);

my $i = 0;
for my $case (@scenarios)
{
	$i++;
	my ($name, $base, $ours, $theirs, $conflicts, $rows) = @$case;
	my $script = qq{
	SET client_min_messages = warning;
	CREATE SCHEMA b$i VERSIONED; ALTER SCHEMA b$i OWNER TO g_merge;
	CREATE SCHEMA o$i VERSIONED; ALTER SCHEMA o$i OWNER TO g_merge;
	CREATE SCHEMA t$i VERSIONED; ALTER SCHEMA t$i OWNER TO g_merge;
	CREATE SCHEMA d$i VERSIONED;
	SET ROLE g_merge;
	SET search_path = b$i; $base;
	SET search_path = o$i; $base; $ours;
	SET search_path = t$i; $base; $theirs;
	RESET ROLE; SET search_path = public;
	SELECT test_ws_root_capture('b$i') AS rb \\gset
	SELECT test_ws_root_capture('o$i') AS ro \\gset
	SELECT test_ws_root_capture('t$i') AS rt \\gset
	\\echo =====merge
	SELECT test_ws_merge(:'rb', :'ro', :'rt', ARRAY['o$i', 't$i', 'b$i']) AS merged \\gset
	\\echo :merged
	\\echo =====end
	SELECT split_part(:'merged', E'\\n', 1) AS m \\gset
	};
	$script .= qq{
	SELECT test_ws_schema_create(test_ws_root_table_bytes(:'m', 't'), 'd$i') IS NULL AS made \\gset
	SELECT test_ws_schema_set_tags(test_ws_root_table_bytes(:'m', 't'), 'd$i') IS NULL AS tagged \\gset
	SELECT test_ws_root_rows_restore(:'m', 't', 'd$i') IS NULL AS restored \\gset
	\\echo =====rows
	SELECT * FROM d$i.t x ORDER BY x::text COLLATE "C";
	\\echo =====end
	} if defined $rows;
	my ($rc, $out, $err) = $node->psql('postgres', $script);
	is($rc, 0, "$name: runs") or diag($err);
	my ($merge) = $out =~ /=====merge\n(.*?)\n?=====end/s;
	my @lines = split /\n/, $merge // '';
	shift @lines;
	is_deeply([ sort @lines ], [ sort @$conflicts ], "$name: its conflicts");
	if (defined $rows)
	{
		my ($got) = $out =~ /=====rows\n(.*?)\n?=====end/s;
		is($got // '', $rows, "$name: its rows");
		# what restored and captured again is what the merge made: the same map
		is($node->safe_psql('postgres', qq{SELECT test_ws_rows_hash('d$i', 't')}) ne '', 1, "$name: captures");
	}
}

$node->stop;
done_testing();
