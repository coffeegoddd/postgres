
# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's catalog rows copied at wal_level minimal: the copies' storage is
# new in the transaction, which skips WAL until commit, and survives a crash
# right after, with what that transaction wrote to it.
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
	CREATE TABLE c.t (a integer PRIMARY KEY, b text, big text);
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

# copied and written in one transaction, a value out of line and a sequence
# moved on, and a crash
my $to2 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('postgres', qq{
	BEGIN;
	SELECT pg_branch_catalog_copy($from, $to2);
	SET LOCAL catalog_branch = $to2;
	INSERT INTO c.t SELECT 2, 'y', string_agg(md5(g::text), '') FROM generate_series(1, 2000) g;
	SELECT nextval('c.s');
	COMMIT;
});
my $read2 = qq{SET catalog_branch = $to2;
	SELECT a, length(big) FROM c.t; SELECT last_value FROM c.s};
is($node->safe_psql('postgres', $read2), "2|64000\n6", 'written in the copying transaction');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $read2), "2|64000\n6", 'and after a crash');

# a copy rolled back to a savepoint leaves nothing for commit to do
my $to3 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('postgres', qq{
	BEGIN;
	SAVEPOINT s;
	SELECT pg_branch_catalog_copy($from, $to3);
	ROLLBACK TO s;
	COMMIT;
});
is( $node->safe_psql('postgres', qq{
		SET catalog_branch = $to3;
		SELECT count(*) FROM pg_namespace WHERE nspname = 'c'}),
	'0',
	'a copy rolled back to a savepoint left nothing');

$node->stop;
done_testing();
