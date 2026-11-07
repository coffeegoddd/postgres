# Copyright (c) 2026, PostgreSQL Global Development Group

# A schema read at an older snapshot, by a transaction at REPEATABLE READ or above, after something
# it names was renamed: what prints a definition reads names as they are now, so it's either as
# the snapshot had it or a serialization failure, never names of now with rows of then.  Inside the
# schema and outside it, and what's outside it that it doesn't name changed with no failure.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('names');
$node->init;
$node->append_conf('postgresql.conf', "log_line_prefix = ''\nautovacuum = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $b = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $b;";
$node->safe_psql('postgres', q{CREATE ROLE objs; GRANT CREATE ON SCHEMA public TO objs; GRANT CREATE ON DATABASE postgres TO objs;
	SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['objs']))});

my %cases = (
	'a column' => ['CREATE TABLE s.t (id integer PRIMARY KEY, a integer CHECK (a > 0))', 'ALTER TABLE s.t RENAME COLUMN a TO b'],
	'a function in a default' => ['CREATE FUNCTION s.f() RETURNS integer LANGUAGE sql IMMUTABLE RETURN 1; CREATE TABLE s.t (id integer PRIMARY KEY, a integer DEFAULT s.f())', 'ALTER FUNCTION s.f() RENAME TO f2'],
	'a function in a check' => ['CREATE FUNCTION s.f(integer) RETURNS boolean LANGUAGE sql IMMUTABLE RETURN $1 > 0; CREATE TABLE s.t (id integer PRIMARY KEY, a integer CHECK (s.f(a)))', 'ALTER FUNCTION s.f(integer) RENAME TO f2'],
	'a function in an index' => ['CREATE FUNCTION s.f(integer) RETURNS integer LANGUAGE sql IMMUTABLE RETURN $1 + 1; CREATE TABLE s.t (id integer PRIMARY KEY, a integer); CREATE INDEX ON s.t (s.f(a))', 'ALTER FUNCTION s.f(integer) RENAME TO f2'],
	'a type of a column' => [q{CREATE TYPE s.e AS ENUM ('x'); CREATE TABLE s.t (id integer PRIMARY KEY, a s.e)}, 'ALTER TYPE s.e RENAME TO e2'],
	'a type in a default' => [q{CREATE TYPE s.e AS ENUM ('x'); CREATE TABLE s.t (id integer PRIMARY KEY, a text DEFAULT 'x'::s.e::text)}, 'ALTER TYPE s.e RENAME TO e2'],
	'an enum value in a default' => [q{CREATE TYPE s.e AS ENUM ('x'); CREATE TABLE s.t (id integer PRIMARY KEY, a s.e DEFAULT 'x')}, q{ALTER TYPE s.e RENAME VALUE 'x' TO 'y'}],
	'a sequence in a default' => ['CREATE SEQUENCE s.q; CREATE TABLE s.t (id integer PRIMARY KEY, a bigint DEFAULT nextval(\'s.q\'))', 'ALTER SEQUENCE s.q RENAME TO q2'],
	'a table a key references' => ['CREATE TABLE s.p (id integer PRIMARY KEY); CREATE TABLE s.t (id integer PRIMARY KEY, a integer REFERENCES s.p)', 'ALTER TABLE s.p RENAME TO p2'],
	'a referenced column' => ['CREATE TABLE s.p (id integer PRIMARY KEY); CREATE TABLE s.t (id integer PRIMARY KEY, a integer REFERENCES s.p (id))', 'ALTER TABLE s.p RENAME COLUMN id TO pid'],
	'a table in a view' => ['CREATE TABLE s.t (id integer PRIMARY KEY); CREATE VIEW s.v AS SELECT id FROM s.t', 'ALTER TABLE s.t RENAME TO t2'],
	'a function of a trigger' => ['CREATE FUNCTION s.tf() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN RETURN NEW; END$$; CREATE TABLE s.t (id integer PRIMARY KEY); CREATE TRIGGER tr BEFORE INSERT ON s.t FOR EACH ROW EXECUTE FUNCTION s.tf()', 'ALTER FUNCTION s.tf() RENAME TO tf2'],
	'a collation of a column' => ['CREATE COLLATION s.c (locale = \'C\'); CREATE TABLE s.t (id integer PRIMARY KEY, a text COLLATE s.c)', 'ALTER COLLATION s.c RENAME TO c2'],
	'a function outside in a default' => ['CREATE FUNCTION public.pf() RETURNS integer LANGUAGE sql IMMUTABLE RETURN 1; CREATE TABLE s.t (id integer PRIMARY KEY, a integer DEFAULT public.pf())', 'ALTER FUNCTION public.pf() RENAME TO pf2'],
	'a type outside of a column' => [q{CREATE TYPE public.pe AS ENUM ('x'); CREATE TABLE s.t (id integer PRIMARY KEY, a public.pe)}, 'ALTER TYPE public.pe RENAME TO pe2'],
	'the schema of a function outside' => ['CREATE SCHEMA other; CREATE FUNCTION other.of() RETURNS integer LANGUAGE sql IMMUTABLE RETURN 1; CREATE TABLE s.t (id integer PRIMARY KEY, a integer DEFAULT other.of())', 'ALTER SCHEMA other RENAME TO other2'],
);

# what changes outside the schema that it doesn't name
my %unnamed = (
	'a function it does not use renamed' => ['CREATE FUNCTION public.pf() RETURNS integer LANGUAGE sql IMMUTABLE RETURN 1; CREATE TABLE s.t (id integer PRIMARY KEY)', 'ALTER FUNCTION public.pf() RENAME TO pf2'],
	'a table outside written' => ['CREATE TABLE public.po (a integer); CREATE TABLE s.t (id integer PRIMARY KEY)', 'INSERT INTO public.po VALUES (1)'],
);

# A root read at a snapshot taken before $change, and one read before it.
sub read_old
{
	my ($setup, $change) = @_;
	$node->safe_psql('postgres', "$on DROP SCHEMA IF EXISTS s CASCADE; DROP SCHEMA IF EXISTS other CASCADE; DROP SCHEMA IF EXISTS other2 CASCADE;
		DROP FUNCTION IF EXISTS public.pf(); DROP FUNCTION IF EXISTS public.pf2(); DROP TYPE IF EXISTS public.pe; DROP TYPE IF EXISTS public.pe2;
		DROP TABLE IF EXISTS public.po; CREATE SCHEMA s VERSIONED; GRANT ALL ON SCHEMA s TO objs; SET ROLE objs; $setup");
	my $before = $node->safe_psql('postgres', "$on SELECT test_ws_root_capture('s')");
	my $old = $node->background_psql('postgres', on_error_stop => 0);
	$old->query_safe("$on BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1");
	$node->safe_psql('postgres', "$on SET ROLE objs; $change");
	$old->{stderr} = '';
	my $got = $old->query("SELECT test_ws_root_capture('s')");
	my $err = $old->{stderr};
	$old->quit;
	return ($before, $got, $err);
}

for my $name (sort keys %cases)
{
	my ($before, $got, $err) = read_old(@{ $cases{$name} });
	like($err, qr/ERROR:  could not read schema "s": it has changed since this transaction's snapshot/, "$name renamed: a serialization failure");
}
for my $name (sort keys %unnamed)
{
	my ($before, $got, $err) = read_old(@{ $unnamed{$name} });
	is($err, '', "$name: no failure");
	is($got, $before, "$name: as at the snapshot");
}

$node->stop;
done_testing();
