# Copyright (c) 2026, PostgreSQL Global Development Group

# The client tools that list a database's tables, for a role that can't reach the history store,
# whose tables they'd otherwise name and be refused.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('client_tools');
$node->init;
$node->start;
$node->safe_psql('postgres', q{
	CREATE ROLE plain LOGIN;
	CREATE ROLE maintainer LOGIN;
	GRANT pg_maintain TO maintainer;
	CREATE TABLE mine (id integer PRIMARY KEY);
	ALTER TABLE mine OWNER TO plain;
});

for my $role (qw(plain maintainer))
{
	$node->command_ok([ 'vacuumdb', '--username' => $role, 'postgres' ], "vacuumdb as $role");
	$node->command_ok([ 'vacuumdb', '--username' => $role, '--full', 'postgres' ], "vacuumdb --full as $role");
	$node->command_ok([ 'vacuumdb', '--username' => $role, '--analyze', 'postgres' ], "vacuumdb --analyze as $role");
}
$node->command_ok([ 'reindexdb', '--username' => 'maintainer', '--jobs' => '2', 'postgres' ], 'reindexdb --jobs as a maintainer');
$node->command_ok([ 'reindexdb', '--username' => 'maintainer', '--jobs' => '2', '--concurrently', 'postgres' ],
	'reindexdb --jobs --concurrently as a maintainer');

# naming the schema, it's told, as for any other it can't reach
$node->command_fails_like([ 'vacuumdb', '--username' => 'plain', '--schema' => 'pg_history', 'postgres' ],
	qr/permission denied for schema pg_history/, 'vacuumdb --schema pg_history as a role that can\'t reach it');

# a role that can reach it still has its tables listed
$node->issues_sql_like([ 'vacuumdb', 'postgres' ], qr/VACUUM .*pg_history\.pointers/, 'vacuumdb as a superuser reaches the store');
$node->issues_sql_like([ 'vacuumdb', '--full', 'postgres' ], qr/VACUUM .*pg_history\.pointers/, 'vacuumdb --full as a superuser reaches the store\'s rewritable tables');
my $log = slurp_file($node->logfile);
unlike($log, qr/VACUUM \([^)]*FULL[^)]*\) pg_history\.chunks/, 'and not the ones it can\'t rewrite');

# with a table named, it's still refused
my ($ret, $out, $err) = $node->psql('postgres', 'VACUUM FULL pg_history.chunks');
like($err, qr/cannot rewrite history store relation "chunks"/, 'naming one is still refused');

$node->stop;
done_testing();
