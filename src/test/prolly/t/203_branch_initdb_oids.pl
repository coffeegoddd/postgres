# Copyright (c) 2026, PostgreSQL Global Development Group

# Object ids below FirstNormalObjectId are initdb's objects', answered as
# shared without reading the catalogs.  A server started in single-user
# mode right after initdb still hands them out, so no versioned schema can
# be made then; once the server has started, one can.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $node = ProllyTest::new_node('initdb_oids');

my ($stdout, $stderr);
IPC::Run::run(
	[ 'postgres', '--single', '-D', $node->data_dir, '-F', 'postgres' ],
	'<' => \"SELECT pg_branch_id_allocate();\nSET catalog_branch = 1;\nCREATE SCHEMA v VERSIONED;\n",
	'>' => \$stdout,
	'2>' => \$stderr);
like($stderr, qr/cannot make versioned schema "v" before the server first starts/,
	'single-user mode right after initdb refuses a versioned schema');

$node->start;
my $b = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
is( $node->safe_psql('postgres',
		"SET catalog_branch = $b; CREATE SCHEMA v VERSIONED; SELECT 'v'::regnamespace::oid >= 16384"),
	't', 'once the server has started, one is made, its object id past initdb\'s');

$node->stop;
done_testing();
