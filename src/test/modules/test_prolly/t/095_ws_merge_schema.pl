# Copyright (c) 2026, PostgreSQL Global Development Group

# A three-way merge of what root values' schemas hold.  Each scenario makes a base, and
# ours and theirs from it, each in a schema of its own (a record does not name its
# schema), and says what conflicts, and what the merged root differs from ours by.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('merge');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$ENV{PGOPTIONS} = "-c catalog_branch=$branch";
$node->safe_psql('postgres', 'CREATE ROLE g_merge NOLOGIN TRAVELS');

# name, base, ours, theirs, conflicts, how the merged root differs from ours
my @scenarios = (
	[ 'both add other columns', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t ADD COLUMN b integer', 'ALTER TABLE t ADD COLUMN c integer',
		[], [ 'column added t.c' ] ],
	[ 'both add one column', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t ADD COLUMN b integer', 'ALTER TABLE t ADD COLUMN b integer',
		[], [] ],
	[ 'one name, two types', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t ADD COLUMN b integer', 'ALTER TABLE t ADD COLUMN b text',
		[ 'column-name t.b' ], undef ],
	[ 'retyped both ways', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t ALTER COLUMN a TYPE bigint', 'ALTER TABLE t ALTER COLUMN a TYPE text',
		[ 'column t.a' ], undef ],
	[ 'retyped alike', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t ALTER COLUMN a TYPE bigint', 'ALTER TABLE t ALTER COLUMN a TYPE bigint',
		[], [] ],
	[ 'renamed both ways', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t RENAME COLUMN a TO b', 'ALTER TABLE t RENAME COLUMN a TO c',
		[ 'column t.b' ], undef ],
	[ 'renamed by one', 'CREATE TABLE t (a integer)',
		'SELECT 1', 'ALTER TABLE t RENAME COLUMN a TO b',
		[], [ 'column renamed t.a b' ] ],
	[ 'renamed and retyped', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t RENAME COLUMN a TO b', 'ALTER TABLE t ALTER COLUMN a TYPE bigint',
		[ 'column t.b' ], undef ],
	[ 'dropped and changed', 'CREATE TABLE t (a integer, b integer)',
		'ALTER TABLE t DROP COLUMN b', 'ALTER TABLE t ALTER COLUMN b SET DEFAULT 1',
		[ 'column t.b' ], undef ],
	[ 'dropped by one', 'CREATE TABLE t (a integer, b integer)',
		'SELECT 1', 'ALTER TABLE t DROP COLUMN b',
		[], [ 'column dropped t.b' ] ],
	[ 'other columns\' other changes', 'CREATE TABLE t (a integer, b integer)',
		'ALTER TABLE t ALTER COLUMN a SET DEFAULT 1', 'ALTER TABLE t ALTER COLUMN b SET NOT NULL',
		[], [ 'column changed t.b' ] ],
	[ 'a column\'s setting by one', 'CREATE TABLE t (a text, b integer)',
		'SELECT 1', 'ALTER TABLE t ALTER COLUMN a SET STORAGE EXTERNAL',
		[], [ 'table changed t' ] ],
	[ 'a column\'s setting both ways', 'CREATE TABLE t (a text)',
		'ALTER TABLE t ALTER COLUMN a SET STORAGE EXTERNAL', 'ALTER TABLE t ALTER COLUMN a SET STORAGE MAIN',
		[ 'column t.a' ], undef ],
	[ 'indexes, other names', 'CREATE TABLE t (a integer, b integer)',
		'CREATE INDEX i1 ON t (a)', 'CREATE INDEX i2 ON t (b)',
		[], [ 'index added t i2' ] ],
	[ 'an index alike', 'CREATE TABLE t (a integer)',
		'CREATE INDEX i1 ON t (a)', 'CREATE INDEX i1 ON t (a)',
		[], [] ],
	[ 'one index name, two indexes', 'CREATE TABLE t (a integer, b integer)',
		'CREATE INDEX i1 ON t (a)', 'CREATE INDEX i1 ON t (b)',
		[ 'table-ddl t' ], undef ],
	[ 'an index on a column the other renames', 'CREATE TABLE t (a integer)',
		'CREATE INDEX i1 ON t (a)', 'ALTER TABLE t RENAME COLUMN a TO b',
		[], [ 'column renamed t.a b' ] ],
	[ 'an index on a column the other drops', 'CREATE TABLE t (a integer, b integer)',
		'CREATE INDEX i1 ON t (b)', 'ALTER TABLE t DROP COLUMN b',
		[ 'table-ddl t' ], undef ],
	[ 'checks, other names', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t ADD CONSTRAINT c1 CHECK (a > 0)', 'ALTER TABLE t ADD CONSTRAINT c2 CHECK (a < 9)',
		[], [ 'constraint added t c2' ] ],
	[ 'one check name, two checks', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t ADD CONSTRAINT c1 CHECK (a > 0)', 'ALTER TABLE t ADD CONSTRAINT c1 CHECK (a < 9)',
		[ 'table-ddl t' ], undef ],
	[ 'a check on a column the other renames', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t ADD CONSTRAINT c1 CHECK (a > 0)', 'ALTER TABLE t RENAME COLUMN a TO b',
		[], [ 'column renamed t.a b' ] ],
	[ 'a check on a column the other drops', 'CREATE TABLE t (a integer, b integer)',
		'ALTER TABLE t ADD CONSTRAINT c1 CHECK (b > 0)', 'ALTER TABLE t DROP COLUMN b',
		[ 'table-ddl t' ], undef ],
	[ 'keys differ', 'CREATE TABLE t (a integer, b integer)',
		'ALTER TABLE t ADD PRIMARY KEY (a)', 'ALTER TABLE t ADD PRIMARY KEY (b)',
		[ 'key t' ], undef ],
	[ 'a key by one', 'CREATE TABLE t (a integer)',
		'SELECT 1', 'ALTER TABLE t ADD PRIMARY KEY (a)',
		[], [ 'key changed t', 'column changed t.a', 'data t same' ] ],
	[ 'a key renamed by one', 'CREATE TABLE t (a integer PRIMARY KEY); CREATE INDEX x ON t (a)',
		'DROP INDEX x', 'ALTER TABLE t RENAME CONSTRAINT t_pkey TO tp',
		[], [ 'key renamed t t_pkey tp' ] ],
	[ 'a key renamed both ways', 'CREATE TABLE t (a integer PRIMARY KEY)',
		'ALTER TABLE t RENAME CONSTRAINT t_pkey TO t1', 'ALTER TABLE t RENAME CONSTRAINT t_pkey TO t2',
		[ 'table-ddl t' ], undef ],
	[ 'a table dropped and changed', 'CREATE TABLE t (a integer)',
		'DROP TABLE t', 'ALTER TABLE t ADD COLUMN b integer',
		[ 'table t' ], undef ],
	[ 'a table dropped by one', 'CREATE TABLE t (a integer); CREATE TABLE u (a integer)',
		'SELECT 1', 'DROP TABLE u',
		[], [ 'table dropped u' ] ],
	[ 'both add a table, other columns', 'SELECT 1',
		'CREATE TABLE t (a integer)', 'CREATE TABLE t (a integer, b integer)',
		[], [ 'column added t.b' ] ],
	[ 'both add a table, a column two ways', 'SELECT 1',
		'CREATE TABLE t (a integer)', 'CREATE TABLE t (a text)',
		[ 'column-name t.a' ], undef ],
	[ 'options, other names', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t SET (fillfactor = 50)', 'ALTER TABLE t SET (autovacuum_enabled = false)',
		[], [ 'table changed t' ] ],
	[ 'an option both ways', 'CREATE TABLE t (a integer)',
		'ALTER TABLE t SET (fillfactor = 50)', 'ALTER TABLE t SET (fillfactor = 60)',
		[ 'table-ddl t' ], undef ],
	[ 'row security by one', 'CREATE TABLE t (a integer)',
		'SELECT 1', 'ALTER TABLE t ENABLE ROW LEVEL SECURITY',
		[], [ 'table changed t' ] ],
	[ 'comments on other things', 'CREATE TABLE t (a integer, b integer)',
		q{COMMENT ON COLUMN t.a IS 'one'}, q{COMMENT ON COLUMN t.b IS 'two'},
		[], [ 'table changed t' ] ],
	[ 'a comment both ways', 'CREATE TABLE t (a integer)',
		q{COMMENT ON COLUMN t.a IS 'one'}, q{COMMENT ON COLUMN t.a IS 'two'},
		[ 'table-ddl t' ], undef ],
	[ 'a comment on a column the other drops', 'CREATE TABLE t (a integer, b integer)',
		q{COMMENT ON COLUMN t.b IS 'one'}, 'ALTER TABLE t DROP COLUMN b',
		[ 'table-ddl t' ], undef ],
	[ 'objects added', 'CREATE TABLE t (a integer)',
		'CREATE VIEW v1 AS SELECT a FROM t', 'CREATE VIEW v2 AS SELECT a FROM t',
		[], [ 'object added view v2' ] ],
	[ 'an object both ways', 'CREATE FUNCTION f(integer) RETURNS integer LANGUAGE sql AS $$SELECT 1$$',
		'CREATE OR REPLACE FUNCTION f(integer) RETURNS integer LANGUAGE sql AS $$SELECT 2$$',
		'CREATE OR REPLACE FUNCTION f(integer) RETURNS integer LANGUAGE sql AS $$SELECT 3$$',
		[ 'object function f(integer)' ], undef ],
	[ 'an object dropped and changed', 'CREATE TABLE t (a integer); CREATE VIEW v AS SELECT a FROM t',
		'DROP VIEW v', 'CREATE OR REPLACE VIEW v AS SELECT a, 1 AS b FROM t',
		[ 'object view v' ], undef ],
	[ 'an enum both ways', q{CREATE TYPE e AS ENUM ('a')},
		q{ALTER TYPE e ADD VALUE 'b'}, q{ALTER TYPE e ADD VALUE 'c'},
		[ 'object type e' ], undef ],
	[ 'a view on a column the other renames', 'CREATE TABLE t (a integer)',
		'CREATE VIEW v AS SELECT a FROM t', 'ALTER TABLE t RENAME COLUMN a TO b',
		[], [ 'column renamed t.a b' ] ],
	[ 'foreign keys, other names', 'CREATE TABLE p (a integer PRIMARY KEY); CREATE TABLE c (a integer, b integer)',
		'ALTER TABLE c ADD CONSTRAINT f1 FOREIGN KEY (a) REFERENCES p (a)',
		'ALTER TABLE c ADD CONSTRAINT f2 FOREIGN KEY (b) REFERENCES p (a)',
		[], [ 'foreign key added c/f2' ] ],
	[ 'one foreign key name, two keys', 'CREATE TABLE p (a integer PRIMARY KEY); CREATE TABLE c (a integer, b integer)',
		'ALTER TABLE c ADD CONSTRAINT f1 FOREIGN KEY (a) REFERENCES p (a)',
		'ALTER TABLE c ADD CONSTRAINT f1 FOREIGN KEY (b) REFERENCES p (a)',
		[ 'object foreign-key c/f1' ], undef ],
	[ 'sequences, the further kept', 'CREATE SEQUENCE sq',
		q{SELECT nextval('sq')}, q{SELECT nextval('sq'); SELECT nextval('sq'); SELECT nextval('sq')},
		[], [ 'sequence changed sq' ] ],
	[ 'sequences, ours further', 'CREATE SEQUENCE sq',
		q{SELECT nextval('sq'); SELECT nextval('sq')}, q{SELECT nextval('sq')},
		[], [] ],
	[ 'a sequence dropped and moved', 'CREATE SEQUENCE sq',
		'DROP SEQUENCE sq', q{SELECT nextval('sq')},
		[], [] ],
	[ 'rows by one', 'CREATE TABLE t (a integer PRIMARY KEY); INSERT INTO t VALUES (1)',
		'SELECT 1', 'INSERT INTO t VALUES (2)',
		[], [ 'data t changed', 'rows t +1 -0 ~0' ] ],
	[ 'rows by one, a rename by the other', 'CREATE TABLE t (a integer PRIMARY KEY, b text); INSERT INTO t VALUES (1, \'x\')',
		'ALTER TABLE t RENAME COLUMN b TO c', 'INSERT INTO t VALUES (2, \'y\')',
		[], [ 'data t changed', 'rows t +1 -0 ~0' ] ],
);

my $i = 0;
my $script = "SET client_min_messages = warning;\n";
for my $case (@scenarios)
{
	$i++;
	my (undef, $base, $ours, $theirs) = @$case;
	$script .= qq{
	CREATE SCHEMA b$i VERSIONED; ALTER SCHEMA b$i OWNER TO g_merge;
	CREATE SCHEMA o$i VERSIONED; ALTER SCHEMA o$i OWNER TO g_merge;
	CREATE SCHEMA t$i VERSIONED; ALTER SCHEMA t$i OWNER TO g_merge;
	SET ROLE g_merge;
	SET search_path = b$i; $base;
	SET search_path = o$i; $base; $ours;
	SET search_path = t$i; $base; $theirs;
	RESET ROLE; SET search_path = public;
	SELECT test_ws_root_capture('b$i') AS rb \\gset
	SELECT test_ws_root_capture('o$i') AS ro \\gset
	SELECT test_ws_root_capture('t$i') AS rt \\gset
	\\echo =====$i
	SELECT test_ws_merge(:'rb', :'ro', :'rt') AS merged \\gset
	\\echo :merged
	SELECT split_part(:'merged', E'\\n', 1) AS m \\gset
	\\echo =====diff
	SELECT test_ws_diff(:'ro', :'m');
	};
}
my $out = $node->safe_psql('postgres', $script);
my %said;
{
	my ($at, $part);
	for my $line (split /\n/, $out)
	{
		if ($line =~ /^=====(\d+)$/) { ($at, $part) = ($1, 'merge'); next; }
		if ($line =~ /^=====(\w+)$/) { $part = $1; next; }
		push @{ $said{$at}{$part} }, $line if defined $at && $line ne '';
	}
}

$i = 0;
for my $case (@scenarios)
{
	$i++;
	my ($name, undef, undef, undef, $conflicts, $diff) = @$case;
	my @merge = @{ $said{$i}{merge} // [] };
	shift @merge;
	is_deeply([ sort @merge ], [ sort @$conflicts ], "$name: its conflicts");
	next unless defined $diff;
	# (what the next scenario's statements print comes after, in no line's form)
	my @lines = sort grep { !/^data \S+ same$/ && !/^rows \S+ \+0 -0 ~0$/ }
	  grep { /^(object|foreign key|sequence|role|table|column|key|constraint|index|data|rows|rewritten) / } @{ $said{$i}{diff} // [] };
	my @want = sort grep { !/^data \S+ same$/ && !/^rows \S+ \+0 -0 ~0$/ } @$diff;
	is_deeply(\@lines, \@want, "$name: and differs from ours by that")
	  or diag(join("\n", 'it said:', @lines));
}

# what merging does not do yet says so
for my $case (
	[ q{CREATE TABLE t (a integer PRIMARY KEY); INSERT INTO t VALUES (1)}, q{INSERT INTO t VALUES (2)}, q{INSERT INTO t VALUES (3)}, qr/merging rows both sides of table "t" changed/ ],
	[ q{CREATE TABLE t (a integer PRIMARY KEY); INSERT INTO t VALUES (1)}, q{SELECT 1}, q{ALTER TABLE t ADD COLUMN b integer DEFAULT 1}, qr/./ ])
{
	my ($base, $ours, $theirs, $expect) = @$case;
	$i++;
	my ($rc, $o, $err) = $node->psql('postgres', qq{
	CREATE SCHEMA b$i VERSIONED; ALTER SCHEMA b$i OWNER TO g_merge;
	CREATE SCHEMA o$i VERSIONED; ALTER SCHEMA o$i OWNER TO g_merge;
	CREATE SCHEMA t$i VERSIONED; ALTER SCHEMA t$i OWNER TO g_merge;
	SET ROLE g_merge;
	SET search_path = b$i; $base;
	SET search_path = o$i; $base; $ours;
	SET search_path = t$i; $base; $theirs;
	RESET ROLE; SET search_path = public;
	SELECT test_ws_root_capture('b$i') AS rb \\gset
	SELECT test_ws_root_capture('o$i') AS ro \\gset
	SELECT test_ws_root_capture('t$i') AS rt \\gset
	SELECT test_ws_merge(:'rb', :'ro', :'rt');
	});
	if ($ours eq 'SELECT 1')
	{
		is($err =~ s/^.*NOTICE.*$//mgr =~ s/\s+//gr, '', 'a column added by one, with its rows, keeps theirs whole');
	}
	else
	{
		like($err, $expect, 'both changing rows says it is not done');
	}
}

$node->stop;
done_testing();
