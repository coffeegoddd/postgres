# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum's fold verifier: at prolly.fold_verify_rate it folds a
# database's tables from their baselines and in full, and compares them.
# A table whose log lost its changes is caught by it, counted, and read in
# full at its next fold, so a commit after takes the right rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fold_verify');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum_naptime = 1s
prolly.fold_verify_rate = 0
});
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA fv VERSIONED;
	CREATE TABLE fv.t (id int PRIMARY KEY, v text);
	INSERT INTO fv.t SELECT g, 'v' || g FROM generate_series(1, 1000) g;
	SELECT dolt_commit('-Am', 'loaded')});

# changes the log then loses, the baseline kept
sql(q{UPDATE fv.t SET v = 'lost' WHERE id <= 20});
sql(q{SELECT test_lose_change_log('fv.t')});
is(sql(q{SELECT count(*) FROM dolt_status}), '0',
	'a fold from the baseline misses the lost changes');

# verification on: autovacuum catches it
$node->append_conf('postgresql.conf', 'prolly.fold_verify_rate = 1');
$node->reload;
ok( $node->poll_query_until(
		'postgres',
		q{SELECT value > 0 FROM pg_stat_prolly WHERE name = 'fold_verify_mismatches'}),
	'autovacuum found the fold from the baseline wrong');
ok($node->log_contains(qr/prolly fold of table fv\.t from its baseline differs/),
	'and warned of it');

# its next fold reads the table in full: the lost changes are back
is(sql(q{SELECT count(*) FROM dolt_status}), '1', 'the table is read in full again');
sql(q{SELECT dolt_commit('-am', 'after')});
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::fv.t) WHERE (to_row).v = 'lost'}),
	'20', 'the commit after has the lost changes');
ok( $node->poll_query_until(
		'postgres',
		q{SELECT value >= 2 FROM pg_stat_prolly WHERE name = 'fold_verified'}),
	'autovacuum goes on checking');
is(sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'fold_verify_mismatches'}),
	'1', 'and finds nothing more wrong');

$node->stop;
done_testing();
