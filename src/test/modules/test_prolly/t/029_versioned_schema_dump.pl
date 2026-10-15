# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_dump keeps a schema versioned, so its tables restore as prolly tables.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vschema_dump');
$node->init;
$node->start;
$node->safe_psql('postgres', q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 100) g;
	CREATE DATABASE restored;
});

my $dump = $node->basedir . '/dump.sql';
command_ok([ 'pg_dump', '--file' => $dump, '--dbname' => $node->connstr('postgres') ],
	'pg_dump');
like(slurp_file($dump), qr/CREATE SCHEMA app VERSIONED;/, 'the dump makes the schema versioned');
command_ok([ 'psql', '--no-psqlrc', '--set' => 'ON_ERROR_STOP=1', '--quiet',
		'--file' => $dump, '--dbname' => $node->connstr('restored') ],
	'the dump restores');
is($node->safe_psql('restored', q{
	SELECT n.nspversioned || ',' || a.amname || ',' || (SELECT count(*) FROM app.t)
	FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
	JOIN pg_am a ON a.oid = c.relam WHERE c.oid = 'app.t'::regclass}),
	'true,prolly,100', 'the restored table is a prolly table in a versioned schema');

$node->stop;
done_testing();
