
# Copyright (c) 2026, PostgreSQL Global Development Group

# Version control finds a reference by its name's key in the pointers table
# whatever the table's statistics: a session's plans made while it held a
# few rows, never analyzed, still read one row a reference once it holds
# thousands (a plan read every row, for every reference, and a commit
# slowed as history grew).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vc_pointer_reads');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	SELECT test_vc_open();
});

# one session: its plans made while the table is small (each statement run
# often enough for its plan to be kept as it is), then thousands of commits,
# then one more, its scans of the table counted
my $out = $node->safe_psql('postgres', q{
	SELECT test_vc_commit('main', 'c' || i) FROM generate_series(1, 20) i;
	SELECT test_vc_log('main') IS NOT NULL;
	SELECT test_vc_bench_commits('main', 3000, 100) > 0;
	BEGIN;
	SELECT test_vc_commit('main', 'last');
	SELECT test_vc_log('main') IS NOT NULL;
	SELECT seq_scan, seq_tup_read, idx_scan > 0
	FROM pg_stat_xact_all_tables WHERE relid = 'pg_history.pointers'::regclass;
	COMMIT;
});
my @lines = split /\n/, $out;
is($node->safe_psql('postgres',
		q{SELECT count(*) > 3000 FROM pg_history.pointers WHERE name LIKE 'vc/commit/%'}),
	't', 'thousands of commits held');
is($node->safe_psql('postgres', q{SELECT reltuples <= 0 FROM pg_class WHERE oid = 'pg_history.pointers'::regclass}),
	't', '(the table never analyzed)');
is($lines[-1], '0|0|t', 'a commit and a log read the pointers by their key alone');

$node->stop;
done_testing();
