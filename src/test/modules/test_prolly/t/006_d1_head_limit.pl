# Copyright (c) 2026, PostgreSQL Global Development Group

# When the cluster's table of branch heads is full, a transaction that
# needs another head fails before it commits, and the server keeps
# running and restarts.  Dropping a database frees its head.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('limit');
$node->init;
$node->append_conf('postgresql.conf', qq(
default_table_access_method = prolly_d1
debug_prolly_max_branch_heads = 3
));
$node->start;

my $rows = 'SELECT count(*) FROM t';
for my $db ('a', 'b', 'c')
{
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, q{
		CREATE TABLE t (id int PRIMARY KEY);
		INSERT INTO t VALUES (1), (2);
	});
}

$node->safe_psql('postgres', 'CREATE DATABASE d');
my ($ret, $stdout, $stderr) = $node->psql('d', q{
	CREATE TABLE t (id int PRIMARY KEY);
	INSERT INTO t VALUES (1);
});
like($stderr, qr/too many prolly branch heads/,
	'a first commit to a fourth database fails');
is($node->safe_psql('d',
	q{SELECT count(*) FROM pg_class WHERE relname = 't'}), '0',
	'and rolls back');

($ret, $stdout, $stderr) =
  $node->psql('postgres', 'CREATE DATABASE e TEMPLATE a');
like($stderr, qr/too many prolly branch heads/,
	'copying a database with prolly data fails too');
is($node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_database WHERE datname = 'e'}), '0',
	'and leaves no database');

is($node->safe_psql('a', 'INSERT INTO t VALUES (3); SELECT count(*) FROM t'),
	'3', 'databases that have a head still commit');

$node->stop('immediate');
$node->start;
for my $db ('a', 'b', 'c')
{
	is($node->safe_psql($db, $rows), $db eq 'a' ? '3' : '2',
		"database $db recovers after a crash at the limit");
}

$node->safe_psql('postgres', 'DROP DATABASE c');
$node->safe_psql('d', q{
	CREATE TABLE t (id int PRIMARY KEY);
	INSERT INTO t VALUES (1);
});
is($node->safe_psql('d', $rows), '1',
	'dropping a database frees its head for another');

$node->restart;
is($node->safe_psql('d', $rows), '1', 'the server restarts cleanly');

done_testing();
