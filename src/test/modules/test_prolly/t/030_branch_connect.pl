# Copyright (c) 2026, PostgreSQL Global Development Group

# In a database with versioned schemas, "dbname/branch" connects on that
# pg_branch row, and each session sees its own branch's versioned objects.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_connect');
$node->init;
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int);
	SET allow_system_table_mods = on;
	INSERT INTO pg_branch VALUES (99999, 'other');
});

is($node->safe_psql('postgres', q{SELECT test_branch_current()}),
	'9427', 'a plain connection is on main');
is($node->safe_psql('postgres/main', q{SELECT test_branch_current()}),
	'9427', 'dbname/main is on main');
is($node->safe_psql('postgres/other', q{SELECT test_branch_current()}),
	'99999', 'dbname/other is on other');
is($node->safe_psql('postgres/other', q{SELECT to_regclass('app.t') IS NULL}),
	't', 'main\'s versioned table isn\'t on other');
is($node->safe_psql('postgres', q{SELECT to_regclass('app.t') IS NULL}),
	'f', 'main still has it');

# Whether other is a pg_branch row doesn't depend on main's schemas.
$node->safe_psql('postgres', q{
	SELECT test_branch_switch('other');
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.o (id int);
});
$node->safe_psql('postgres', q{DROP SCHEMA app CASCADE});
is($node->safe_psql('postgres/other',
		q{SELECT test_branch_current() || ',' || (to_regclass('app.o') IS NOT NULL)}),
	'99999,true', 'dbname/other works when main has no versioned schema');

my ($ret, $stdout, $stderr) =
  $node->psql('postgres/nope', q{SELECT 1});
isnt($ret, 0, 'connecting to a branch that doesn\'t exist fails');
like($stderr, qr/branch "nope" does not exist/, 'and says why');

$node->stop;
done_testing();
