# Copyright (c) 2026, PostgreSQL Global Development Group

# What each change does to the diff, and to stored rows: every step's diff against
# the step before it, line by line, in one backend so each root is still there.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('diff');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$ENV{PGOPTIONS} = "-c catalog_branch=$branch";

# steps: [ name, sql, what it says ], all of it but that a table's data is the same
# and none of its rows changed, unless named
my @steps = (
	[ 'rename a column', q{ALTER TABLE s.t RENAME COLUMN v TO w},
		[ 'column renamed t.v w', 'data t same', 'rows t +0 -0 ~0' ] ],
	[ 'add a nullable column', q{ALTER TABLE s.t ADD COLUMN extra text},
		[ 'column added t.extra', 'data t same' ] ],
	[ 'add a column with a default', q{ALTER TABLE s.t ADD COLUMN flag integer DEFAULT 7},
		[ 'column added t.flag', 'data t changed', 'rows t +0 -0 ~0', 'rewritten t 500' ] ],
	[ 'retype, the text unchanged', q{ALTER TABLE s.t ALTER COLUMN n TYPE bigint},
		[ 'column retyped t.n integer -> bigint', 'data t same' ] ],
	[ 'retype, the text changed', q{ALTER TABLE s.t ALTER COLUMN n TYPE numeric(10,2)},
		[ 'column retyped t.n bigint -> numeric(10,2)', 'data t changed', 'rows t +0 -0 ~500' ] ],
	[ 'drop a column that is all NULL', q{ALTER TABLE s.t DROP COLUMN z},
		[ 'column dropped t.z', 'data t same' ] ],
	[ 'drop a column with values', q{ALTER TABLE s.t DROP COLUMN flag},
		[ 'column dropped t.flag', 'data t changed', 'rows t +0 -0 ~0', 'rewritten t 500' ] ],
	[ 'add an index', q{CREATE INDEX t_w ON s.t (w)},
		[ 'index added t t_w', 'data t same' ] ],
	[ 'change an index', q{DROP INDEX s.t_w; CREATE UNIQUE INDEX t_w ON s.t (w, id)},
		[ 'index changed t t_w', 'data t same' ] ],
	[ 'drop an index', q{DROP INDEX s.t_w},
		[ 'index dropped t t_w', 'data t same' ] ],
	[ 'add a check', q{ALTER TABLE s.t ADD CONSTRAINT c1 CHECK (id > 0)},
		[ 'constraint added t c1', 'data t same' ] ],
	[ 'change a check', q{ALTER TABLE s.t DROP CONSTRAINT c1; ALTER TABLE s.t ADD CONSTRAINT c1 CHECK (id > -1)},
		[ 'constraint changed t c1', 'data t same' ] ],
	[ 'drop a check', q{ALTER TABLE s.t DROP CONSTRAINT c1},
		[ 'constraint dropped t c1', 'data t same' ] ],
	[ 'change rows', q{UPDATE s.t SET w = 'x' WHERE id <= 10; DELETE FROM s.t WHERE id > 490; INSERT INTO s.t (id, w, n) VALUES (9001, 'new', 1)},
		[ 'data t changed', 'rows t +1 -10 ~10' ] ],
	[ 'drop a column and add it back', q{ALTER TABLE s.t DROP COLUMN extra; ALTER TABLE s.t ADD COLUMN extra text},
		[ ] ],
	[ 'set a default', q{ALTER TABLE s.t ALTER COLUMN w SET DEFAULT 'd'},
		[ 'column default t.w', 'data t same' ] ],
	[ 'set not null', q{ALTER TABLE s.t ALTER COLUMN w SET NOT NULL},
		[ 'column changed t.w', 'data t same' ] ],
	[ 'a table setting', q{ALTER TABLE s.t SET (fillfactor = 70)},
		[ 'table changed t', 'data t same' ] ],
	[ 'a large value', q{UPDATE s.t SET w = repeat('l', 100000) WHERE id = 20},
		[ 'data t changed', 'rows t +0 -0 ~1' ] ],
	[ 'a large value the same', q{UPDATE s.t SET w = repeat('l', 100000) WHERE id = 20},
		[ 'data t same', 'rows t +0 -0 ~0' ] ],
	[ 'a large value changed', q{UPDATE s.t SET w = repeat('l', 99999) || 'm' WHERE id = 20},
		[ 'data t changed', 'rows t +0 -0 ~1' ] ],
	[ 'nothing', q{SELECT 1}, [ ] ],
	[ 'a column moved', q{ALTER TABLE s.mv DROP COLUMN b; ALTER TABLE s.mv ADD COLUMN b text},
		[ 'column moved mv', 'data mv changed', 'rows mv +0 -0 ~2' ] ],
	[ 'a key renamed', q{ALTER TABLE s.mv RENAME CONSTRAINT mv_pkey TO mv_key2},
		[ 'key renamed mv mv_pkey mv_key2' ] ],
	[ 'an identity', q{ALTER TABLE s.mv ALTER COLUMN c ADD GENERATED ALWAYS AS IDENTITY},
		[ 'column changed mv.c', 'sequence added mv_c_seq' ] ],
	[ 'a collation', q{ALTER TABLE s.mv ALTER COLUMN b TYPE text COLLATE "C"},
		[ 'column changed mv.b' ] ],
	[ 'a keyless large value', q{INSERT INTO s.kl VALUES (repeat('a', 5000)), (repeat('a', 5000)); INSERT INTO s.kl VALUES (repeat('b', 5000))},
		[ 'data kl changed', 'rows kl +3 -0 ~0' ] ],
	[ 'a keyless large value goes', q{DELETE FROM s.kl WHERE v = repeat('b', 5000)},
		[ 'data kl changed', 'rows kl +0 -1 ~0' ] ],
	[ 'rows of no columns', q{INSERT INTO s.nc DEFAULT VALUES; INSERT INTO s.nc DEFAULT VALUES},
		[ 'data nc changed', 'rows nc +2 -0 ~0' ] ],
	[ 'no column in common', q{ALTER TABLE s.nc2 ADD COLUMN b integer DEFAULT 1; ALTER TABLE s.nc2 DROP COLUMN a},
		[ 'column added nc2.b', 'column dropped nc2.a', 'data nc2 changed', 'rows nc2 +0 -0 ~0', 'rewritten nc2 3' ] ],
	[ 'a name not in ASCII', q{CREATE TABLE s."tàbé" ("cól" integer); INSERT INTO s."tàbé" VALUES (1)},
		[ 'table added tàbé' ] ],
	[ 'its column renamed', q{ALTER TABLE s."tàbé" RENAME COLUMN "cól" TO "cöl"},
		[ 'column renamed tàbé.cól cöl' ] ],
	[ 'a role', q{RESET ROLE; CREATE ROLE g_other NOLOGIN TRAVELS},
		[ 'role added g_other' ] ],
	[ 'a role dropped', q{RESET ROLE; DROP ROLE g_other},
		[ 'role dropped g_other' ] ],
	[ 'keyless: add a nullable column', q{ALTER TABLE s.k ADD COLUMN c integer},
		[ 'column added k.c', 'data k same' ] ],
	[ 'keyless: rename a column', q{ALTER TABLE s.k RENAME COLUMN b TO bb},
		[ 'column renamed k.b bb', 'data k same' ] ],
	[ 'keyless: add a column with a default', q{ALTER TABLE s.k ADD COLUMN d integer DEFAULT 1},
		[ 'column added k.d', 'data k changed', 'rows k +0 -0 ~0', 'rewritten k 200' ] ],
	[ 'keyless: change rows', q{DELETE FROM s.k WHERE a = 5; INSERT INTO s.k VALUES (500, 'x', NULL, 1), (500, 'x', NULL, 1)},
		[ 'data k changed', 'rows k +2 -2 ~0' ] ],
	[ 'keyless: one of many the same goes', q{DELETE FROM s.k WHERE ctid = (SELECT min(ctid) FROM s.k WHERE a = 500)},
		[ 'data k changed', 'rows k +0 -1 ~0' ] ],
	[ 'keyless: drop a column, with rows changed too', q{ALTER TABLE s.k DROP COLUMN d; INSERT INTO s.k VALUES (501, 'y', NULL)},
		[ 'column dropped k.d', 'data k changed', 'rows k +1 -0 ~0', 'rewritten k 199' ] ],
	[ 'a key made, every row other', q{ALTER TABLE s.t2 ADD PRIMARY KEY (a)},
		[ 'column changed t2.a', 'key changed t2', 'data t2 changed', 'rows t2 +5 -5 ~0' ] ],
	[ 'a key changed', q{ALTER TABLE s.t2 DROP CONSTRAINT t2_pkey; ALTER TABLE s.t2 ADD PRIMARY KEY (b)},
		[ 'column changed t2.b', 'key changed t2', 'data t2 changed', 'rows t2 +5 -5 ~0' ] ],
	[ 'a key dropped', q{ALTER TABLE s.t2 DROP CONSTRAINT t2_pkey},
		[ 'key changed t2', 'data t2 changed', 'rows t2 +5 -5 ~0' ] ],
	[ 'a table added', q{CREATE TABLE s.nw (a integer)}, [ 'table added nw' ] ],
	[ 'a table dropped', q{DROP TABLE s.nw}, [ 'table dropped nw' ] ],
	[ 'objects', q{CREATE VIEW s.vw AS SELECT id FROM s.t; CREATE FUNCTION s.fn(integer) RETURNS integer LANGUAGE sql AS 'SELECT 1'},
		[ 'object added view vw', 'object added function fn(integer)' ] ],
	[ 'an object changed', q{CREATE OR REPLACE FUNCTION s.fn(integer) RETURNS integer LANGUAGE sql AS 'SELECT 2'},
		[ 'object changed function fn(integer)' ] ],
	[ 'an object dropped', q{DROP VIEW s.vw},
		[ 'object dropped view vw' ] ],
	[ 'a rename an object follows', q{CREATE VIEW s.vw2 AS SELECT w FROM s.t},
		[ 'object added view vw2' ] ],
	[ 'its column renamed', q{ALTER TABLE s.t RENAME COLUMN w TO w2},
		[ 'column renamed t.w w2', 'data t same' ] ],
	[ 'a foreign key', q{CREATE TABLE s.fkc (a integer REFERENCES s.t (id))},
		[ 'table added fkc', 'foreign key added fkc/fkc_a_fkey' ] ],
	[ 'a foreign key changed', q{ALTER TABLE s.fkc DROP CONSTRAINT fkc_a_fkey; ALTER TABLE s.fkc ADD CONSTRAINT fkc_a_fkey FOREIGN KEY (a) REFERENCES s.t (id) ON DELETE CASCADE},
		[ 'foreign key changed fkc/fkc_a_fkey' ] ],
	[ 'a foreign key dropped', q{ALTER TABLE s.fkc DROP CONSTRAINT fkc_a_fkey},
		[ 'foreign key dropped fkc/fkc_a_fkey' ] ],
	[ 'a sequence', q{CREATE SEQUENCE s.sq},
		[ 'object added sequence sq', 'sequence added sq' ] ],
	[ 'a sequence moved', q{SELECT nextval('s.sq')},
		[ 'sequence changed sq' ] ],
	[ 'a partition\'s rows', q{UPDATE s.p SET r = 5 WHERE id = 1},
		[ 'data p same', 'data pa changed', 'rows pa +1 -0 ~0', 'data pb changed', 'rows pb +0 -1 ~0' ] ],
);

my $script = q{
	CREATE ROLE g_diff NOLOGIN TRAVELS;
	CREATE SCHEMA s VERSIONED;
	ALTER SCHEMA s OWNER TO g_diff;
	SET ROLE g_diff;
	CREATE TABLE s.t (id integer PRIMARY KEY, v text, n integer, z text);
	INSERT INTO s.t SELECT g, 'v' || g, g, NULL FROM generate_series(1, 500) g;
	CREATE TABLE s.k (a integer, b text);
	INSERT INTO s.k SELECT g % 100, 'b' || g % 7 FROM generate_series(1, 200) g;
	CREATE TABLE s.t2 (a integer, b integer);
	INSERT INTO s.t2 SELECT g, g + 100 FROM generate_series(1, 5) g;
	CREATE TABLE s.mv (id integer PRIMARY KEY, b text, c integer NOT NULL);
	INSERT INTO s.mv VALUES (1, 'x', 1), (2, 'y', 2);
	CREATE TABLE s.kl (v text);
	CREATE TABLE s.nc ();
	CREATE TABLE s.nc2 (a integer); INSERT INTO s.nc2 VALUES (1), (2), (3);
	CREATE TABLE s.p (id integer, r integer) PARTITION BY RANGE (r);
	CREATE TABLE s.pa PARTITION OF s.p FOR VALUES FROM (0) TO (10);
	CREATE TABLE s.pb PARTITION OF s.p FOR VALUES FROM (10) TO (20);
	INSERT INTO s.p VALUES (1, 15), (2, 15), (3, 5);
	RESET ROLE;
	SELECT test_ws_root_capture('s') AS r0 \gset
};
my $i = 0;
for my $step (@steps)
{
	my $from = $i++;
	$script .= qq{\n\\echo =====$i\nSET ROLE g_diff;\n$step->[1];\nRESET ROLE;\nSELECT test_ws_root_capture('s') AS r$i \\gset\nSELECT test_ws_diff(:'r$from', :'r$i');\n};
}
my $out = $node->safe_psql('postgres', $script);
my %said;
{
	my $at;
	for my $line (split /\n/, $out)
	{
		if ($line =~ /^=====(\d+)$/) { $at = $1; $said{$at} = []; next; }
		push @{ $said{$at} }, $line if defined $at;
	}
}
$i = 0;
for my $step (@steps)
{
	$i++;
	my ($name, undef, $want) = @$step;
	my @said = sort grep { !/^data \S+ same$/ && !/^rows \S+ \+0 -0 ~0$/ }
	  grep { /^(object|foreign key|sequence|role|table|column|key|constraint|index|data|rows|rewritten) / } @{ $said{$i} };
	my @wanted = sort grep { !/^data \S+ same$/ && !/^rows \S+ \+0 -0 ~0$/ } @$want;
	is_deeply(\@said, \@wanted, $name) or diag(join("\n", 'it said:', @said));
}

# a root against itself has every table's lines, and nothing changed
my $self = $node->safe_psql('postgres', q{SELECT test_ws_root_capture('s') AS r \gset
	SELECT test_ws_diff(:'r', :'r')});
like($self, qr/^data t same$/m, 'a root against itself says what it read');
my @changed = grep { !/^data \S+ same$/ && !/^rows \S+ \+0 -0 ~0$/ && $_ ne '' } split /\n/, $self;
is("@changed", '', 'and that nothing changed');

$node->stop;
done_testing();
