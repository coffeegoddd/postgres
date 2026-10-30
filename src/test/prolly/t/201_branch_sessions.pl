# Copyright (c) 2026, PostgreSQL Global Development Group

# Two sessions on two branches of one versioned schema run DDL and DML at
# once, each seeing only its own definitions and rows plus the shared
# schema's; pg_dump answers per branch.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $node = ProllyTest::new_node('sessions');
$node->start;

my $b1 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b2 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');

sub on
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', "SET catalog_branch = $branch; $sql");
}

$node->safe_psql('postgres',
	'CREATE SCHEMA pub; CREATE TABLE pub.p (id int PRIMARY KEY, note text);
	 INSERT INTO pub.p VALUES (1, \'shared\')');
for my $b ($b1, $b2)
{
	on($b,
		"CREATE SCHEMA app VERSIONED;
		 CREATE TABLE app.t (id int PRIMARY KEY, v int, p int REFERENCES pub.p);
		 INSERT INTO app.t SELECT g, g, 1 FROM generate_series(1, $b * 10) g;");
}

# both sessions hold their DDL open at once, on tables of the same name
my $s1 = $node->background_psql('postgres');
my $s2 = $node->background_psql('postgres');
$s1->query_safe("SET catalog_branch = $b1");
$s2->query_safe("SET catalog_branch = $b2");
$s1->query_safe('BEGIN');
$s2->query_safe('BEGIN');
$s1->query_safe('CREATE INDEX t_v1 ON app.t (v)');
$s2->query_safe('ALTER TABLE app.t ADD COLUMN extra int DEFAULT 7');
$s1->query_safe('CREATE TABLE app.only (x int)');
$s2->query_safe('CREATE TABLE app.only (y int, z int)');
$s1->query_safe('INSERT INTO app.t VALUES (1001, 1, 1)');
$s2->query_safe('INSERT INTO app.t VALUES (1001, 2, 1, 8)');
$s1->query_safe("INSERT INTO pub.p VALUES (2, 'from one')");
$s2->query_safe("INSERT INTO pub.p VALUES (3, 'from two')");
$s1->query_safe('COMMIT');
$s2->query_safe('COMMIT');

is(on($b1, 'SELECT count(*) FROM app.t'), $b1 * 10 + 1, 'rows on the first branch');
is(on($b2, 'SELECT count(*) FROM app.t'), $b2 * 10 + 1, 'rows on the second branch');
is( on(
		$b1,
		"SELECT string_agg(column_name, ',' ORDER BY ordinal_position)
		 FROM information_schema.columns WHERE table_schema = 'app' AND table_name = 't'"),
	'id,v,p',
	'the first branch has its own columns');
is( on(
		$b2,
		"SELECT string_agg(column_name, ',' ORDER BY ordinal_position)
		 FROM information_schema.columns WHERE table_schema = 'app' AND table_name = 't'"),
	'id,v,p,extra',
	'the second branch has its own columns');
is( on(
		$b1,
		"SELECT string_agg(indexname, ',' ORDER BY indexname) FROM pg_indexes WHERE schemaname = 'app'"),
	't_pkey,t_v1',
	'the first branch has its own indexes');
is( on(
		$b2,
		"SELECT string_agg(indexname, ',' ORDER BY indexname) FROM pg_indexes WHERE schemaname = 'app'"),
	't_pkey',
	'the second branch has its own indexes');
is(on($b1, 'SELECT count(*) FROM app.only, LATERAL (SELECT x) s'), 0,
	'the same table name made on both branches: the first');
is(on($b2, 'SELECT y, z FROM app.only'), '', 'and the second');
is(on($b1, 'SELECT string_agg(id::text, \',\' ORDER BY id) FROM pub.p'), '1,2,3',
	'both branches write the shared table');

# each session's catalog queries after the other's DDL, in one session
# that stays open across it
$s1->query_safe("SELECT count(*) FROM pg_class WHERE relname = 'only'");
$s2->query_safe('CREATE TABLE app.later (a int)');
is($s1->query_safe("SELECT count(*) FROM pg_class WHERE relname IN ('only', 'later')"),
	1, 'another branch\'s new table stays hidden from an open session');
is($s2->query_safe("SELECT count(*) FROM pg_class WHERE relname IN ('only', 'later')"),
	2, 'and its own session sees it');
$s1->quit;
$s2->quit;

# pg_dump answers per branch
my $dump = sub {
	my ($branch) = @_;
	local $ENV{PGOPTIONS} = "-c catalog_branch=$branch";
	my ($stdout, $stderr);
	IPC::Run::run([ 'pg_dump', '-s', '-n', 'app', '-d', $node->connstr('postgres') ],
		'>', \$stdout, '2>', \$stderr);
	return $stdout;
};
my $d1 = $dump->($b1);
my $d2 = $dump->($b2);
like($d1, qr/CREATE INDEX t_v1/, 'the first branch\'s dump has its index');
unlike($d1, qr/extra/, 'and not the other\'s column');
like($d2, qr/extra integer DEFAULT 7/, 'the second branch\'s dump has its column');
unlike($d2, qr/t_v1/, 'and not the other\'s index');
unlike($dump->(0), qr/CREATE TABLE/, 'a dump on no branch has no versioned table');
like($d1, qr/CREATE SCHEMA app VERSIONED;/, 'a versioned schema is dumped as one');

# a new branch made from the first branch's dump, definitions and rows
my $b3 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my ($full, $err);
{
	local $ENV{PGOPTIONS} = "-c catalog_branch=$b1";
	IPC::Run::run([ 'pg_dump', '-n', 'app', '-d', $node->connstr('postgres') ],
		'>', \$full, '2>', \$err);
}
{
	local $ENV{PGOPTIONS} = "-c catalog_branch=$b3";
	my ($out, $rerr);
	IPC::Run::run([ 'psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres') ],
		'<', \$full, '>', \$out, '2>', \$rerr);
	is($rerr, '', 'the dump restores on a new branch');
}
is(on($b3, 'SELECT count(*), sum(v) FROM app.t'),
	on($b1, 'SELECT count(*), sum(v) FROM app.t'),
	'the new branch has the first branch\'s rows');
is( on(
		$b3,
		"SELECT string_agg(indexname, ',' ORDER BY indexname) FROM pg_indexes WHERE schemaname = 'app'"),
	't_pkey,t_v1',
	'and its definitions');
is(on($b1, 'SELECT count(*) FROM app.t'), $b1 * 10 + 1, 'the first branch is as it was');
{
	local $ENV{PGOPTIONS} = "-c catalog_branch=0";
	my ($out, $rerr);
	IPC::Run::run([ 'psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres') ],
		'<', \$full, '>', \$out, '2>', \$rerr);
	like($rerr, qr/cannot create versioned schema "app" on no branch/,
		'and it cannot be restored on no branch');
}

$node->stop;
done_testing();
