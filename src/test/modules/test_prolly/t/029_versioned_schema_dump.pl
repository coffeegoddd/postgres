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

# A versioned public schema replaces initdb's; so does its restore.
$node->safe_psql('postgres', q{
	CREATE DATABASE pubsrc;
	CREATE DATABASE pubdst;
});
$node->safe_psql('pubsrc', q{
	DROP SCHEMA public;
	CREATE SCHEMA public VERSIONED;
	GRANT USAGE ON SCHEMA public TO PUBLIC;
	CREATE TABLE public.t (id integer);
	INSERT INTO public.t VALUES (1);
});
my $pubdump = $node->basedir . '/pubdump.sql';
command_ok([ 'pg_dump', '--file' => $pubdump, '--dbname' => $node->connstr('pubsrc') ],
	'pg_dump of a versioned public schema');
command_ok([ 'psql', '--no-psqlrc', '--set' => 'ON_ERROR_STOP=1', '--quiet',
		'--file' => $pubdump, '--dbname' => $node->connstr('pubdst') ],
	'the dump of a versioned public schema restores');
my $pubcheck = q{
	SELECT n.nspversioned || ',' || a.amname || ',' || (SELECT count(*) FROM public.t)
	  || ',' || has_schema_privilege('public', 'public', 'USAGE')
	FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
	JOIN pg_am a ON a.oid = c.relam WHERE c.oid = 'public.t'::regclass};
is($node->safe_psql('pubdst', $pubcheck), 'true,prolly,1,true',
	'the restored public schema is versioned, with its prolly table and grants');

# An extension in a versioned schema: the dump makes the schema versioned
# before it makes the extension there, whose objects are shared again.
$node->safe_psql('postgres', q{
	CREATE DATABASE extsrc;
	CREATE DATABASE extdst;
});
$node->safe_psql('extsrc', q{
	DROP SCHEMA public;
	CREATE SCHEMA public VERSIONED;
	CREATE EXTENSION citext SCHEMA public;
	CREATE TABLE public.t (id integer PRIMARY KEY, name public.citext);
	INSERT INTO public.t VALUES (1, 'Hi');
});
my $extdump = $node->basedir . '/extdump.sql';
command_ok([ 'pg_dump', '--file' => $extdump, '--dbname' => $node->connstr('extsrc') ],
	'pg_dump of an extension in a versioned schema');
command_ok([ 'psql', '--no-psqlrc', '--set' => 'ON_ERROR_STOP=1', '--quiet',
		'--file' => $extdump, '--dbname' => $node->connstr('extdst') ],
	'the dump of an extension in a versioned schema restores');
is($node->safe_psql('extdst', q{
	SELECT n.nspversioned || ',' || e.extname || ',' ||
	  (SELECT typbranch = 0 FROM pg_type WHERE typname = 'citext'
	     AND typnamespace = 'public'::regnamespace) || ',' ||
	  (SELECT count(*) FROM public.t WHERE name = 'HI'::public.citext)
	FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace
	WHERE e.extname = 'citext'}),
	'true,citext,true,1',
	'the restored extension is in the versioned schema, shared, and its table reads');

$node->stop;
done_testing();
