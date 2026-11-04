
# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's catalog rows copied at wal_level minimal: the copies' storage is
# new in the transaction, which skips WAL and syncs it at commit, and survives
# a crash right after.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('copy_minimal');
$node->init;
$node->append_conf('postgresql.conf', "wal_level = minimal\nmax_wal_senders = 0\n");
$node->start;
is($node->safe_psql('postgres', 'SHOW wal_level'), 'minimal', 'at wal_level minimal');

my $from = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $to = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('postgres', qq{
	SET catalog_branch = $from;
	CREATE SCHEMA c VERSIONED;
	CREATE TABLE c.t (a integer PRIMARY KEY, b text);
	CREATE INDEX ON c.t (b);
	CREATE SEQUENCE c.s;
	SELECT nextval('c.s') FROM generate_series(1, 5);
});
$node->safe_psql('postgres', "SELECT pg_branch_catalog_copy($from, $to)");

my $read = qq{SET catalog_branch = $to; SET enable_seqscan = off;
	SELECT count(*) FROM c.t WHERE a = 1; SELECT last_value FROM c.s};
is($node->safe_psql('postgres', $read), "0\n5", 'the copy reads, its sequence with it');

$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $read), "0\n5", 'and after a crash');

$node->safe_psql('postgres', qq{
	SET catalog_branch = $to;
	INSERT INTO c.t VALUES (1, 'x');
});
is( $node->safe_psql('postgres', qq{
		SET catalog_branch = $to; SET enable_seqscan = off;
		SELECT b FROM c.t WHERE a = 1}),
	'x',
	'and takes rows, its indexes too');

$node->stop;
done_testing();
