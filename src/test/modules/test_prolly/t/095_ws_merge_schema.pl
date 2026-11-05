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
$node->safe_psql('postgres', 'CREATE ROLE g_merge NOLOGIN TRAVELS; CREATE ROLE g_other NOLOGIN TRAVELS; CREATE ROLE g_third NOLOGIN TRAVELS; GRANT g_other TO g_merge');

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
		[], [ 'column dropped t.b', 'index dropped t i1' ] ],
	[ 'an expression index on a column the other drops', 'CREATE TABLE t (a integer, b integer)',
		'CREATE INDEX i1 ON t ((b + 1)); CREATE INDEX i2 ON t (a); ALTER INDEX i2 SET (fillfactor = 50); ALTER TABLE t ADD COLUMN z integer', 'ALTER TABLE t DROP COLUMN b',
		[], [ 'column dropped t.b', 'index dropped t i1' ] ],
	[ 'a commented index on a column the other drops', 'CREATE TABLE t (a integer, b integer)',
		q{CREATE INDEX i1 ON t (b); COMMENT ON INDEX i1 IS 'it'}, 'ALTER TABLE t DROP COLUMN b',
		[], [ 'column dropped t.b', 'index dropped t i1', 'table changed t' ] ],
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
	[ 'both add a table otherwise', 'SELECT 1',
		'CREATE TABLE t (a integer)', 'CREATE TABLE t (a integer, b integer)',
		[ 'table t' ], undef ],
	[ 'both add a table alike', 'SELECT 1',
		'CREATE TABLE t (a integer)', 'CREATE TABLE t (a integer)',
		[], [] ],
	[ 'a key both change alike', 'CREATE TABLE t (a integer, b integer)',
		'ALTER TABLE t ADD PRIMARY KEY (a); ALTER TABLE t ADD COLUMN c integer', 'ALTER TABLE t ADD PRIMARY KEY (a); ALTER TABLE t ADD COLUMN d integer',
		[ 'key t' ], undef ],
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
	[ 'a descending sequence, the further kept', 'CREATE SEQUENCE sq INCREMENT BY -1',
		q{SELECT nextval('sq')}, q{SELECT nextval('sq'); SELECT nextval('sq'); SELECT nextval('sq')},
		[], [ 'sequence changed sq' ] ],
	[ 'a descending sequence, ours further', 'CREATE SEQUENCE sq INCREMENT BY -1',
		q{SELECT nextval('sq'); SELECT nextval('sq')}, q{SELECT nextval('sq')},
		[], [] ],
	[ 'sequences, ours further', 'CREATE SEQUENCE sq',
		q{SELECT nextval('sq'); SELECT nextval('sq')}, q{SELECT nextval('sq')},
		[], [] ],
	[ 'a sequence dropped and moved', 'CREATE SEQUENCE sq',
		'DROP SEQUENCE sq', q{SELECT nextval('sq')},
		[], [] ],
	# each side changes something, so none is taken whole
	[ 'owner by one', 'CREATE TABLE t (a integer, b integer)',
		'ALTER TABLE t ALTER COLUMN a SET DEFAULT 1', 'ALTER TABLE t OWNER TO g_other; ALTER TABLE t ALTER COLUMN b SET DEFAULT 1',
		[], [ 'column default t.b', 'table changed t' ] ],
	[ 'one privilege by each', 'CREATE TABLE t (a integer)',
		'GRANT SELECT ON t TO g_other', 'GRANT INSERT ON t TO g_other',
		[ 'table-ddl t' ], undef ],
	[ 'grants to other roles', 'CREATE TABLE t (a integer)',
		'GRANT SELECT ON t TO g_other', 'GRANT SELECT ON t TO g_third',
		[], [ 'table changed t' ] ],
	[ 'replica identity by one', 'CREATE TABLE t (a integer PRIMARY KEY, b integer)',
		'ALTER TABLE t ALTER COLUMN b SET DEFAULT 1', 'ALTER TABLE t REPLICA IDENTITY FULL; ALTER TABLE t ADD COLUMN c integer',
		[], [ 'column added t.c', 'table changed t' ] ],
	[ 'replica identity both ways', 'CREATE TABLE t (a integer PRIMARY KEY)',
		'ALTER TABLE t REPLICA IDENTITY FULL', 'ALTER TABLE t REPLICA IDENTITY NOTHING',
		[ 'table-ddl t' ], undef ],
	[ 'clustered by one', 'CREATE TABLE t (a integer PRIMARY KEY, b integer)',
		'ALTER TABLE t ALTER COLUMN b SET DEFAULT 1', 'ALTER TABLE t CLUSTER ON t_pkey',
		[], [ 'table changed t' ] ],
	[ 'clustered on an index the other drops', 'CREATE TABLE t (a integer, b integer); CREATE INDEX i ON t (b)',
		'ALTER TABLE t CLUSTER ON i', 'ALTER TABLE t DROP COLUMN b',
		[ 'table-ddl t' ], undef ],
	[ 'an index setting by one', 'CREATE TABLE t (a integer, b integer); CREATE INDEX i ON t (a)',
		'ALTER TABLE t ALTER COLUMN b SET DEFAULT 1', 'ALTER INDEX i SET (fillfactor = 50)',
		[], [ 'table changed t' ] ],
	[ 'a parent both add to', 'CREATE TABLE p (a integer); CREATE TABLE c (b integer) INHERITS (p)',
		'ALTER TABLE p ADD COLUMN x integer', 'ALTER TABLE p ADD COLUMN y integer',
		[], [ 'column added p.y', 'column added c.y', 'table changed c' ] ],
	[ 'a parent\'s check each adds', 'CREATE TABLE p (a integer); CREATE TABLE c (b integer) INHERITS (p)',
		'ALTER TABLE p ADD CONSTRAINT c1 CHECK (a > 0)', 'ALTER TABLE p ADD CONSTRAINT c2 CHECK (a < 9)',
		[], [ 'constraint added p c2', 'constraint added c c2', 'table changed c' ] ],
	[ 'a column moved under what a child inherits', 'CREATE TABLE p (a integer, b integer); CREATE TABLE c (d integer) INHERITS (p)',
		'ALTER TABLE p ADD COLUMN x integer', 'ALTER TABLE c ADD COLUMN e integer',
		[], [ 'column added c.e' ] ],
	[ 'a new child of a parent the other adds to', 'CREATE TABLE p (a integer)',
		'CREATE TABLE c (b integer) INHERITS (p)', 'ALTER TABLE p ADD COLUMN x integer',
		[ 'table-ddl c' ], undef ],
	[ 'a new partition of a parent the other adds to', 'CREATE TABLE p (a integer) PARTITION BY RANGE (a)',
		'CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (9)', 'ALTER TABLE p ADD COLUMN x integer',
		[ 'table-ddl p1' ], undef ],
	[ 'a new partition of a parent the other drops', 'CREATE TABLE p (a integer) PARTITION BY RANGE (a); CREATE TABLE u (a integer)',
		'CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (9)', 'DROP TABLE p',
		[ 'table-ddl p1' ], undef ],
	[ 'partitions by each', 'CREATE TABLE p (a integer) PARTITION BY RANGE (a)',
		'CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (9)', 'CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (9) TO (19)',
		[], [ 'table added p2' ] ],
	[ 'a table and a view of one name', 'CREATE TABLE u (a integer)',
		'CREATE TABLE x (a integer)', 'CREATE VIEW x AS SELECT a FROM u',
		[ 'table-ddl x' ], undef ],
	[ 'a table and an index of one name', 'CREATE TABLE u (a integer)',
		'CREATE TABLE x (a integer)', 'CREATE INDEX x ON u (a)',
		[ 'table-ddl x' ], undef ],
	[ 'a foreign key on a column the other drops', 'CREATE TABLE p (a integer PRIMARY KEY); CREATE TABLE c (a integer, b integer)',
		'ALTER TABLE c ADD CONSTRAINT f1 FOREIGN KEY (b) REFERENCES p (a)', 'ALTER TABLE c DROP COLUMN b',
		[], [ 'column dropped c.b', 'foreign key dropped c/f1' ] ],
	[ 'a foreign key to a table the other drops', 'CREATE TABLE p (a integer PRIMARY KEY); CREATE TABLE c (a integer)',
		'ALTER TABLE c ADD CONSTRAINT f1 FOREIGN KEY (a) REFERENCES p (a)', 'DROP TABLE p',
		[], [ 'table dropped p', 'foreign key dropped c/f1' ] ],
	[ 'a foreign key on a table the other drops', 'CREATE TABLE p (a integer PRIMARY KEY); CREATE TABLE c (a integer)',
		'ALTER TABLE c ADD CONSTRAINT f1 FOREIGN KEY (a) REFERENCES p (a)', 'DROP TABLE c',
		[], [ 'table dropped c', 'foreign key dropped c/f1' ] ],
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
	CREATE SCHEMA b$i VERSIONED; ALTER SCHEMA b$i OWNER TO g_merge; GRANT ALL ON SCHEMA b$i TO g_other;
	CREATE SCHEMA o$i VERSIONED; ALTER SCHEMA o$i OWNER TO g_merge; GRANT ALL ON SCHEMA o$i TO g_other;
	CREATE SCHEMA t$i VERSIONED; ALTER SCHEMA t$i OWNER TO g_merge; GRANT ALL ON SCHEMA t$i TO g_other;
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

# a side's rows with what merging leaves them otherwise: says it is not done, or is kept
sub rows_case
{
	my ($base, $ours, $theirs) = @_;
	$i++;
	return $node->psql('postgres', qq{
	CREATE SCHEMA b$i VERSIONED; ALTER SCHEMA b$i OWNER TO g_merge; GRANT ALL ON SCHEMA b$i TO g_other;
	CREATE SCHEMA o$i VERSIONED; ALTER SCHEMA o$i OWNER TO g_merge; GRANT ALL ON SCHEMA o$i TO g_other;
	CREATE SCHEMA t$i VERSIONED; ALTER SCHEMA t$i OWNER TO g_merge; GRANT ALL ON SCHEMA t$i TO g_other;
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
}

my $moving = qr/moving rows of table "t" into its merged columns is not done yet/;
for my $case (
	[ 'both change rows', q{CREATE TABLE t (a integer PRIMARY KEY); INSERT INTO t VALUES (1)},
		q{INSERT INTO t VALUES (2)}, q{INSERT INTO t VALUES (3)}, qr/merging rows both sides of table "t" changed/ ],
	# a retype keeping the text: the other side's new rows have not met the type
	[ 'a retype one side, new rows of the old type the other', q{CREATE TABLE t (a integer PRIMARY KEY, b text); INSERT INTO t VALUES (1, '5')},
		q{ALTER TABLE t ALTER COLUMN b TYPE integer USING b::integer}, q{INSERT INTO t VALUES (2, 'not a number')}, $moving ],
	[ 'a narrower type one side, longer rows the other', q{CREATE TABLE t (a integer PRIMARY KEY, b varchar(10)); INSERT INTO t VALUES (1, 'x')},
		q{ALTER TABLE t ALTER COLUMN b TYPE varchar(3)}, q{INSERT INTO t VALUES (2, 'longer')}, $moving ],
	[ 'another expression one side, new rows the other', q{CREATE TABLE t (a integer PRIMARY KEY, g integer GENERATED ALWAYS AS (a + 1) STORED); INSERT INTO t VALUES (1)},
		q{ALTER TABLE t ALTER COLUMN g SET EXPRESSION AS (a + 2)}, q{INSERT INTO t VALUES (2)}, qr/merging rows both sides of table "t" changed/ ],
	[ 'a default added one side, new rows the other', q{CREATE TABLE t (a integer PRIMARY KEY); INSERT INTO t VALUES (1)},
		q{ALTER TABLE t ADD COLUMN c integer DEFAULT 1}, q{INSERT INTO t VALUES (2)}, qr/merging rows both sides of table "t" changed/ ],
	[ 'a column with values dropped one side, new rows the other', q{CREATE TABLE t (a integer PRIMARY KEY, b integer); INSERT INTO t VALUES (1, 1)},
		q{ALTER TABLE t DROP COLUMN b}, q{INSERT INTO t VALUES (2, 2)}, qr/merging rows both sides of table "t" changed/ ],
	# rows one side alone holds, a column the other added with a default not in them
	[ 'a default added one side, the same rows the other, a default for all', q{CREATE TABLE t (a integer PRIMARY KEY); INSERT INTO t VALUES (1)},
		q{CREATE INDEX i ON t (a)}, q{ALTER TABLE t ADD COLUMN c integer DEFAULT 1}, undef ])
{
	my ($name, $base, $ours, $theirs, $expect) = @$case;
	my ($rc, $o, $err) = rows_case($base, $ours, $theirs);
	if (defined $expect)
	{
		like($err, $expect, "$name: not done yet");
	}
	else
	{
		is($rc, 0, "$name: theirs taken with its rows") or diag($err);
	}
}
for my $case (
	[ 'a retype keeping the text, with no other rows', q{CREATE TABLE t (a integer PRIMARY KEY, b text); INSERT INTO t VALUES (1, '5')},
		q{ALTER TABLE t ALTER COLUMN b TYPE integer USING b::integer}, q{ALTER TABLE t ALTER COLUMN a SET DEFAULT 1} ],
	[ 'an index one side, a nullable column the other', q{CREATE TABLE t (a integer PRIMARY KEY); INSERT INTO t VALUES (1)},
		q{CREATE INDEX i ON t (a)}, q{ALTER TABLE t ADD COLUMN c integer} ],
	[ 'a nullable column one side, an index the other', q{CREATE TABLE t (a integer PRIMARY KEY); INSERT INTO t VALUES (1)},
		q{ALTER TABLE t ADD COLUMN c integer}, q{CREATE INDEX i ON t (a)} ],
	[ 'a nullable column one side, new rows the other', q{CREATE TABLE t (a integer PRIMARY KEY); INSERT INTO t VALUES (1)},
		q{ALTER TABLE t ADD COLUMN c integer}, q{INSERT INTO t VALUES (2)} ],
	[ 'an all-null column dropped one side, its rows unchanged the other', q{CREATE TABLE t (a integer PRIMARY KEY, b integer, c integer); INSERT INTO t VALUES (1)},
		q{ALTER TABLE t DROP COLUMN b}, q{ALTER TABLE t ALTER COLUMN c SET DEFAULT 1} ])
{
	my ($name, $base, $ours, $theirs) = @$case;
	my ($rc, $o, $err) = rows_case($base, $ours, $theirs);
	is($rc, 0, "$name: merges") or diag($err);
}

$node->stop;
done_testing();
