# Copyright (c) 2026, PostgreSQL Global Development Group

# A table's rows as its row map, and back: each table's own, a partition's under
# its own name; one set of rows one map; written as held, running no user code.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('rows');
$node->init;
$node->start;
$node->safe_psql('postgres', q{CREATE EXTENSION test_prolly; CREATE ROLE grp NOLOGIN; CREATE ROLE stranger LOGIN IN ROLE grp});
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$ENV{PGOPTIONS} = "-c catalog_branch=$branch";

sub hash_of
{
	my ($schema, $table, $db) = @_;
	return $node->safe_psql($db // 'postgres', qq{SELECT test_ws_rows_hash('$schema', '$table')});
}

# the table made in dst, as its record says, and tagged
sub make_table
{
	my ($table, $db) = @_;
	$node->safe_psql($db // 'postgres', qq{SELECT test_ws_schema_create(test_ws_schema_bytes('s', '$table'), 'd'); SELECT test_ws_schema_set_tags(test_ws_schema_bytes('s', '$table'), 'd')});
	return;
}

sub copy_table
{
	my ($table, $db) = @_;
	make_table($table, $db);
	$node->safe_psql($db // 'postgres', qq{SELECT test_ws_rows_copy('s', '$table', 'd')});
	return;
}

sub same_rows
{
	my ($table, $order, $name) = @_;
	is($node->safe_psql('postgres', qq{SELECT * FROM d.$table ORDER BY $order}),
		$node->safe_psql('postgres', qq{SELECT * FROM s.$table ORDER BY $order}), "$name: the same rows");
	is(hash_of('d', $table), hash_of('s', $table), "$name: the same map");
	return;
}

$node->safe_psql('postgres', q{CREATE SCHEMA s VERSIONED; CREATE SCHEMA d VERSIONED; CREATE SCHEMA s2 VERSIONED});
my ($rc, $out, $err);

# values of many types, NULLs, a dropped column, and a key of two
$node->safe_psql('postgres', q{
	CREATE TABLE s.kv (id integer, k2 text, gone integer, t text, j jsonb, ts timestamptz, f double precision,
		a integer[], b bytea, n numeric(10, 2), d date, i interval, nothing text, PRIMARY KEY (id, k2));
	ALTER TABLE s.kv DROP COLUMN gone;
	INSERT INTO s.kv VALUES
		(1, 'a', 'tab	and
newline \ back', '{"x": [1, 2]}', '2026-01-02 03:04:05+00', 0.1, '{1,NULL,3}', '\x00ff', 1.50, '2026-01-02', '1 day 2 hours', NULL),
		(2, 'é', '', 'null', NULL, 'NaN', '{}', '', NULL, NULL, NULL, NULL),
		(3, 'c', 'NULL', NULL, NULL, '-Infinity', NULL, NULL, -0.01, 'infinity', NULL, NULL)});
copy_table('kv');
same_rows('kv', 'id', 'keyed rows');

# rows the same, and rows of nothing
$node->safe_psql('postgres', q{
	CREATE TABLE s.nk (a integer, b text);
	INSERT INTO s.nk VALUES (1, 'x'), (1, 'x'), (1, 'x'), (2, 'y'), (NULL, NULL), (NULL, NULL), (1, NULL)});
copy_table('nk');
same_rows('nk', 'a, b', 'keyless rows');
is($node->safe_psql('postgres', q{SELECT count(*) FROM d.nk}), '7', 'each of the same rows there');

# the same rows across batches are one entry, as within one
$node->safe_psql('postgres', q{
	CREATE TABLE s.many (a integer, b text);
	INSERT INTO s.many SELECT i % 7, 'v' FROM generate_series(1, 5000) i;});
is(hash_of('s', 'many'), do {
	$node->safe_psql('postgres', q{CREATE TABLE s2.many (a integer, b text); INSERT INTO s2.many SELECT i % 7, 'v' FROM generate_series(1, 5000) i ORDER BY i % 7 DESC});
	hash_of('s2', 'many') }, 'a keyless table is one map whichever order, across batches');
copy_table('many');
same_rows('many', 'a, b', 'keyless rows over batches');
is($node->safe_psql('postgres', q{SELECT count(*) FROM d.many}), '5000', 'each row there');

# no rows
$node->safe_psql('postgres', q{CREATE TABLE s.none (a integer PRIMARY KEY, b text)});
copy_table('none');
same_rows('none', 'a', 'no rows');

# nothing but a key, and no stored column at all
$node->safe_psql('postgres', q{
	CREATE TABLE s.keyonly (a integer PRIMARY KEY); INSERT INTO s.keyonly VALUES (1), (2);
	CREATE TABLE s.nocols (); INSERT INTO s.nocols DEFAULT VALUES; INSERT INTO s.nocols DEFAULT VALUES});
copy_table('keyonly');
same_rows('keyonly', 'a', 'a key alone');
copy_table('nocols');
is($node->safe_psql('postgres', q{SELECT count(*) FROM d.nocols}), '2', 'rows of no columns, each there');
is(hash_of('d', 'nocols'), hash_of('s', 'nocols'), 'and the same map');

# types whose text hangs on settings, or could
$node->safe_psql('postgres', q{
	CREATE TYPE s.en AS ENUM ('a', 'b'); CREATE TYPE d.en AS ENUM ('a', 'b');
	CREATE TABLE s.typed (id integer PRIMARY KEY, m money, e s.en, z double precision);
	INSERT INTO s.typed VALUES (1, 12.5, 'b', '-0')});
is($node->safe_psql('postgres', q{SET extra_float_digits = 0; SELECT test_ws_rows_hash('s', 'typed')}),
	hash_of('s', 'typed'), '-0, with money and an enum, is one map under other float digits');
copy_table('typed');
is($node->safe_psql('postgres', q{SELECT id, m, e::text, z::text FROM d.typed}), '1|$12.50|b|-0', 'and restored as they were');

# large values, in a key's row and a keyless one
$node->safe_psql('postgres', q{
	CREATE TABLE s.big (id integer PRIMARY KEY, v text, w text);
	INSERT INTO s.big SELECT i, (SELECT string_agg(md5(i::text || g::text), '') FROM generate_series(1, 3000) g), 'short' FROM generate_series(1, 3) i;
	INSERT INTO s.big VALUES (4, repeat('x', 2048), repeat('y', 2049));
	CREATE TABLE s.bigk (v text);
	INSERT INTO s.bigk SELECT v FROM s.big; INSERT INTO s.bigk SELECT v FROM s.big WHERE id = 1});
copy_table('big');
same_rows('big', 'id', 'large values');
copy_table('bigk');
same_rows('bigk', 'md5(v)', 'large values, keyless');

# one set of rows is one map, in any order under any settings
$node->safe_psql('postgres', q{
	CREATE TABLE s.order1 (id integer PRIMARY KEY, ts timestamptz, f double precision, i interval, b bytea);
	CREATE TABLE s2.order1 (id integer PRIMARY KEY, ts timestamptz, f double precision, i interval, b bytea);
	INSERT INTO s.order1 SELECT i, '2026-01-02 03:04:05+00', i * 0.1, '1 day', '\xab' FROM generate_series(1, 2000) i;
	INSERT INTO s2.order1 SELECT i, '2026-01-02 03:04:05+00', i * 0.1, '1 day', '\xab' FROM generate_series(2000, 1, -1) i;
	UPDATE s2.order1 SET f = f WHERE id % 3 = 0});
my $h1 = hash_of('s', 'order1');
is($h1, hash_of('s2', 'order1'), 'rows in another order are one map');
is($node->safe_psql('postgres', q{SET DateStyle = 'SQL, DMY'; SET TimeZone = 'Asia/Tokyo'; SET IntervalStyle = 'sql_standard'; SET extra_float_digits = -3; SET bytea_output = 'escape'; SELECT test_ws_rows_hash('s', 'order1')}),
	$h1, 'under other settings, one map');

# what leaves rows alone leaves the map alone
my $before = hash_of('s', 'kv');
$node->safe_psql('postgres', q{ALTER TABLE s.kv RENAME COLUMN t TO t2; ALTER TABLE s.kv ADD COLUMN extra text});
is(hash_of('s', 'kv'), $before, 'a rename and a nullable column leave the map');
$node->safe_psql('postgres', q{ALTER TABLE s.kv DROP COLUMN extra; ALTER TABLE s.kv RENAME COLUMN t2 TO t});

# stored generated values as held, not computed; a virtual one is no row's
$node->safe_psql('postgres', q{
	CREATE FUNCTION s.fn(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1 * 2';
	CREATE FUNCTION d.fn(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1 * 3';
	CREATE TABLE s.gen (id integer PRIMARY KEY, x integer, g integer GENERATED ALWAYS AS (s.fn(x)) STORED, v integer GENERATED ALWAYS AS (x + 1) VIRTUAL);
	INSERT INTO s.gen (id, x) VALUES (1, 10), (2, 20)});
copy_table('gen');
is($node->safe_psql('postgres', q{SELECT id, x, g, v FROM d.gen ORDER BY id}), "1|10|20|11\n2|20|40|21", 'a stored generated value is what was held');
is(hash_of('d', 'gen'), hash_of('s', 'gen'), 'and the same map');

# no trigger fires, no domain checks, and indexes have the rows
$node->safe_psql('postgres', q{
	CREATE FUNCTION d.no() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RAISE EXCEPTION ''fired''; END';
	CREATE DOMAIN s.dom AS integer; CREATE DOMAIN d.dom AS integer CHECK (VALUE < 0);
	CREATE TABLE s.dm (id integer PRIMARY KEY, x s.dom, u text);
	INSERT INTO s.dm VALUES (1, 5, 'one'), (2, 6, 'two')});
make_table('dm');
$node->safe_psql('postgres', q{CREATE TRIGGER t BEFORE INSERT ON d.dm FOR EACH ROW EXECUTE FUNCTION d.no(); CREATE UNIQUE INDEX dmu ON d.dm (u)});
$node->safe_psql('postgres', q{SELECT test_ws_rows_copy('s', 'dm', 'd')});
is($node->safe_psql('postgres', q{SELECT x FROM d.dm ORDER BY id}), "5\n6", 'no trigger fired and no domain checked');
is($node->safe_psql('postgres', q{SET enable_seqscan = off; SELECT id FROM d.dm WHERE u = 'two'}), '2', 'a later index has them too');
($rc, $out, $err) = $node->psql('postgres', q{ALTER TABLE d.dm DISABLE TRIGGER t; INSERT INTO d.dm VALUES (9, -1, 'one')});
like($err, qr/duplicate key/, 'and enforces what it should');

# a domain's checks do not run at any depth: in an array, a composite or a range
$node->safe_psql('postgres', q{
	CREATE DOMAIN s.pos AS integer CHECK (VALUE > 0); CREATE DOMAIN d.pos AS integer CHECK (VALUE < 0);
	CREATE TYPE s.cmp AS (p s.pos); CREATE TYPE d.cmp AS (p d.pos);
	CREATE TYPE s.rng AS RANGE (subtype = s.pos); CREATE TYPE d.rng AS RANGE (subtype = d.pos);
	CREATE TABLE s.deep (id integer PRIMARY KEY, a s.pos[], c s.cmp, r s.rng);
	INSERT INTO s.deep VALUES (1, '{5}', ROW(5), '[1,5)')});
copy_table('deep');
is($node->safe_psql('postgres', q{SELECT a, c, r FROM d.deep}), '{5}|(5)|[1,5)', 'no domain checked in an array, composite or range');
# in one session, after a restore, and after one that failed reading a value in
$node->safe_psql('postgres', q{CREATE TABLE s.badval (id integer PRIMARY KEY, x text); INSERT INTO s.badval VALUES (1, 'abc')});
make_table('badval');
$node->safe_psql('postgres', q{ALTER TABLE d.badval ALTER COLUMN x TYPE integer USING 0; TRUNCATE d.badval});
$node->safe_psql('postgres', q{CREATE TABLE s.okval (id integer PRIMARY KEY); INSERT INTO s.okval VALUES (1)});
make_table('okval');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_rows_copy('s', 'okval', 'd'); SELECT '{5}'::d.pos[]});
like($err, qr/violates check constraint/, 'checks are back after a restore, in its session');
($rc, $out, $err) = $node->psql('postgres', q{
	DO $$ BEGIN PERFORM test_ws_rows_copy('s', 'badval', 'd'); RAISE 'it read'; EXCEPTION WHEN invalid_text_representation THEN NULL; END $$;
	SELECT '{5}'::d.pos[]});
like($err, qr/violates check constraint/, 'and after one that failed reading a value');
unlike($err, qr/it read/, 'which did fail');

# an index's functions see a domain's checks as they always do, though what is read in does not
$node->safe_psql('postgres', q{
	CREATE DOMAIN s.ip AS integer CHECK (VALUE > 0);
	CREATE FUNCTION s.safe(integer) RETURNS integer LANGUAGE plpgsql IMMUTABLE AS $$
	BEGIN RETURN (('{' || $1 || '}')::s.ip[])[1]; EXCEPTION WHEN check_violation THEN RETURN 0; END $$;
	CREATE TABLE s.ixf (id integer PRIMARY KEY, x integer);
	INSERT INTO s.ixf VALUES (1, -5)});
make_table('ixf');
$node->safe_psql('postgres', q{CREATE INDEX ixf_safe ON d.ixf (s.safe(x)); SELECT test_ws_rows_copy('s', 'ixf', 'd')});
is($node->safe_psql('postgres', q{SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT id FROM d.ixf WHERE s.safe(x) = 0}), '1', 'an index expression relying on a domain indexes what it should');

# keys long enough that few fit a node: a tree of them is still one that reads
$node->safe_psql('postgres', q{
	CREATE TABLE s.lk (k text PRIMARY KEY, v integer);
	INSERT INTO s.lk SELECT (SELECT string_agg(md5(i::text || g::text), '') FROM generate_series(1, 78) g), i FROM generate_series(1, 500) i});
copy_table('lk');
same_rows('lk', 'v', 'long keys');

# a key's rows while a deferrable key lets two be: refused, not one row of two
$node->safe_psql('postgres', q{
	CREATE TABLE s.dk (id integer PRIMARY KEY DEFERRABLE, v text);
	INSERT INTO s.dk VALUES (1, 'a'), (2, 'b')});
copy_table('dk');
same_rows('dk', 'id', 'a deferrable key');
($rc, $out, $err) = $node->psql('postgres', q{BEGIN; SET CONSTRAINTS ALL DEFERRED; UPDATE s.dk SET id = 1 WHERE id = 2; SELECT test_ws_rows_hash('s', 'dk'); ROLLBACK});
like($err, qr/has rows whose keys print alike, 2 rows to 1 keys/, 'capture refuses two rows of a key');
# and what is restored under a deferrable index is checked now
$node->safe_psql('postgres', q{CREATE TABLE s.dk2 (id integer, v text, UNIQUE (id) DEFERRABLE);
	CREATE TABLE s2.dk2 (id integer, v text); INSERT INTO s2.dk2 VALUES (1, 'a'), (1, 'b')});
make_table('dk2');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_rows_copy('s2', 'dk2', 'd')});
like($err, qr/has a key another has, under a deferrable constraint/, 'a deferrable unique index refuses what it would at commit');

# a read-only transaction writes nothing
$node->safe_psql('postgres', q{CREATE TABLE s.ro (a integer); INSERT INTO s.ro VALUES (1)});
make_table('ro');
($rc, $out, $err) = $node->psql('postgres', q{BEGIN READ ONLY; SELECT test_ws_rows_copy('s', 'ro', 'd'); COMMIT});
like($err, qr/cannot execute restoring rows in a read-only transaction/, 'a read-only transaction is refused');
is($node->safe_psql('postgres', q{SELECT count(*) FROM d.ro}), '0', 'and nothing written');

# a partitioned table holds none; each leaf its own, whichever way it divides
$node->safe_psql('postgres', q{
	CREATE TABLE s.p (id integer, r integer, v text, PRIMARY KEY (id, r)) PARTITION BY RANGE (r);
	CREATE TABLE s.p1 PARTITION OF s.p FOR VALUES FROM (0) TO (100);
	CREATE TABLE s.p2 PARTITION OF s.p FOR VALUES FROM (100) TO (200) PARTITION BY LIST (id);
	CREATE TABLE s.p2a PARTITION OF s.p2 FOR VALUES IN (1, 2);
	CREATE TABLE s.p2b PARTITION OF s.p2 DEFAULT;
	CREATE TABLE s.pd PARTITION OF s.p DEFAULT;
	CREATE TABLE s.pe PARTITION OF s.p FOR VALUES FROM (500) TO (600);
	INSERT INTO s.p SELECT i, CASE WHEN i % 4 = 0 THEN 50 WHEN i % 4 = 1 THEN 150 WHEN i % 4 = 2 THEN 150 ELSE 300 END, 'v' || i FROM generate_series(1, 40) i;
	CREATE TABLE s.kp (a integer, b text) PARTITION BY HASH (a);
	CREATE TABLE s.kp0 PARTITION OF s.kp FOR VALUES WITH (MODULUS 2, REMAINDER 0);
	CREATE TABLE s.kp1 PARTITION OF s.kp FOR VALUES WITH (MODULUS 2, REMAINDER 1);
	INSERT INTO s.kp SELECT i % 5, 'x' FROM generate_series(1, 30) i});
my $empty = hash_of('s', 'none');
is(hash_of('s', 'p'), $empty, 'a partitioned table holds no rows, with rows under it');
is(hash_of('s', 'p2'), $empty, 'nor one partition of another that is partitioned');
is(hash_of('s', 'pe'), $empty, 'a leaf with none is empty');
isnt(hash_of('s', $_), $empty, "leaf $_ holds its rows") for qw(p1 p2a p2b pd kp0 kp1);
isnt(hash_of('s', 'p1'), hash_of('s', 'pd'), 'each leaf its own rows');

copy_table($_) for qw(p p1 p2 p2a p2b pd pe kp kp0 kp1);
for my $case (['p', 'p1'], ['p', 'p2'], ['p2', 'p2a'], ['p2', 'p2b'], ['p', 'pd'], ['p', 'pe'], ['kp', 'kp0'], ['kp', 'kp1'])
{
	my ($parent, $leaf) = @$case;
	$node->safe_psql('postgres', qq{SELECT test_ws_schema_attach(test_ws_schema_bytes('s', '$leaf'), 'd')});
}
for my $table (qw(p kp))
{
	my $order = $table eq 'p' ? 'id, r' : 'a, b';
	is($node->safe_psql('postgres', qq{SELECT tableoid::regclass::text, * FROM d.$table ORDER BY $order}) =~ s/^d\.//mgr,
		$node->safe_psql('postgres', qq{SELECT tableoid::regclass::text, * FROM s.$table ORDER BY $order}) =~ s/^s\.//mgr,
		"$table: every row in the same leaf, attached");
	is(hash_of('d', $table), $empty, "$table: still holds none itself");
}
is(hash_of('d', $_), hash_of('s', $_), "$_: the same map, attached") for qw(p1 p2a p2b pd pe kp0 kp1);

# a row that moves is gone from one map and new in the other
my ($p1_was, $pd_was) = (hash_of('s', 'p1'), hash_of('s', 'pd'));
$node->safe_psql('postgres', q{UPDATE s.p SET r = 50 WHERE id = 3});
isnt(hash_of('s', 'p1'), $p1_was, 'a row moved in changes the leaf it came to');
isnt(hash_of('s', 'pd'), $pd_was, 'and the leaf it left');
$node->safe_psql('postgres', q{UPDATE s.p SET r = 300 WHERE id = 3});
is(hash_of('s', 'p1'), $p1_was, 'moved back, one map again');
is(hash_of('s', 'pd'), $pd_was, 'on both sides');

# an inheriting table's rows are its own, not its parent's
$node->safe_psql('postgres', q{
	CREATE TABLE s.par (a integer);
	CREATE TABLE s.chi (b integer) INHERITS (s.par);
	INSERT INTO s.par VALUES (1); INSERT INTO s.chi VALUES (2, 3);
	CREATE TABLE s2.par (a integer); INSERT INTO s2.par VALUES (1)});
is(hash_of('s', 'par'), hash_of('s2', 'par'), 'a parent\'s map is its own rows');

# refusals
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_rows_copy_as('s', 'p1', 'p', 'd')});
like($err, qr/partitioned table "p" holds rows of its own/, 'a partitioned table\'s map with rows is refused');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_rows_copy('s', 'p1', 'd')});
like($err, qr/cannot be restored into "p1" while it is attached/, 'an attached partition is refused');

$node->safe_psql('postgres', q{CREATE TABLE s.own (a integer); INSERT INTO s.own VALUES (1); GRANT USAGE ON SCHEMA s, d TO grp; GRANT SELECT ON s.own TO grp});
make_table('own');
$node->safe_psql('postgres', q{GRANT INSERT ON d.own TO grp});
($rc, $out, $err) = $node->psql('postgres', q{SET ROLE stranger; SELECT test_ws_rows_copy('s', 'own', 'd')});
like($err, qr/must be owner of table own/, 'only its owner writes past its checks');

$node->safe_psql('postgres', q{CREATE TABLE s.shape (a integer, b integer); INSERT INTO s.shape VALUES (1, 2)});
make_table('shape');
$node->safe_psql('postgres', q{ALTER TABLE d.shape ADD COLUMN c integer});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_rows_copy('s', 'shape', 'd')});
like($err, qr/column "c" of table "shape" is not one its rows hold/, 'a column more than it holds is refused');
$node->safe_psql('postgres', q{ALTER TABLE d.shape DROP COLUMN c; ALTER TABLE d.shape DROP COLUMN b});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_rows_copy('s', 'shape', 'd')});
like($err, qr/has 1 of the 2 columns its rows hold/, 'a column fewer too');

# one value in any encoding is one map
$node->safe_psql('postgres', q{CREATE DATABASE l TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C'});
my $postgres_options = $ENV{PGOPTIONS};
my $l_branch;
{
	local $ENV{PGOPTIONS} = '';
	$node->safe_psql('l', q{CREATE EXTENSION test_prolly});
	$l_branch = $node->safe_psql('l', 'SELECT pg_branch_id_allocate()');
}
$node->safe_psql('postgres', q{CREATE TABLE s.enc (id integer PRIMARY KEY, t text); INSERT INTO s.enc VALUES (1, 'é')});
my $utf8_hash = hash_of('s', 'enc');
$ENV{PGOPTIONS} = "-c catalog_branch=$l_branch";
$node->safe_psql('l', q{SET client_encoding = 'UTF8'; CREATE SCHEMA s VERSIONED; CREATE SCHEMA d VERSIONED; CREATE TABLE s.enc (id integer PRIMARY KEY, t text); INSERT INTO s.enc VALUES (1, 'é')});
is($node->safe_psql('l', q{SELECT octet_length(t) FROM s.enc}), '1', 'held there in its own encoding');
is(hash_of('s', 'enc', 'l'), $utf8_hash, 'a value in LATIN1 is the map of it in UTF-8');
copy_table('enc', 'l');
is($node->safe_psql('l', q{SELECT ascii(t) FROM d.enc}), '233', 'and restored there as that character');
$ENV{PGOPTIONS} = $postgres_options;

$node->stop;
done_testing();
