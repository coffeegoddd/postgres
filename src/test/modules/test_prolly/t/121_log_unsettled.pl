# Copyright (c) 2026, PostgreSQL Global Development Group

# The change finder "log", folding while other transactions' writes are in
# flight: their entries in the change log aren't seen by the fold, so they
# stay for the next, which counts those that committed meanwhile (and not
# those that didn't), with a key and without, reading only the entries.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('log_unsettled');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->append_conf('postgresql.conf', 'debug_prolly_change_finder = log');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA lu VERSIONED;
	CREATE TABLE lu.k (id int PRIMARY KEY, v text);
	CREATE TABLE lu.h (n int, v text);
	INSERT INTO lu.k SELECT g, 'base' FROM generate_series(1, 5000) g;
	INSERT INTO lu.h SELECT g, 'base' FROM generate_series(1, 5000) g;
	SELECT dolt_commit('-Am', 'base');
});

# Writes in flight while a commit folds: one to commit later, one to roll
# back.
my $bg = $node->background_psql('postgres');
$bg->query_safe(q{BEGIN;
	INSERT INTO lu.k VALUES (-1, 'committed later');
	UPDATE lu.k SET v = 'committed later' WHERE id = 10;
	INSERT INTO lu.h VALUES (-1, 'committed later');
	DELETE FROM lu.h WHERE n = 10});
my $bg2 = $node->background_psql('postgres');
$bg2->query_safe(q{BEGIN;
	INSERT INTO lu.k VALUES (-2, 'rolled back');
	DELETE FROM lu.k WHERE id = 20;
	INSERT INTO lu.h VALUES (-2, 'rolled back');
	DELETE FROM lu.h WHERE n = 20});
sql(q{INSERT INTO lu.k VALUES (0, 'mine'); INSERT INTO lu.h VALUES (0, 'mine');
	SELECT dolt_commit('-am', 'mine')});
$bg->query_safe(q{COMMIT});
$bg2->query_safe(q{ROLLBACK});

# The next fold reads the entries left, and is the full fold's tree.
my @out = split /\n/, sql(q{
	SELECT test_fold_counters(true);
	SELECT test_fold_root() AS inc \\gset
	SELECT candidates || ' ' || full_tables FROM test_fold_counters(true);
	SELECT :'inc' = test_fold_root(true)});
is($out[1], '4 0', 'reading only the entries left, neither table in full');
is($out[2], 't', 'the fold counts what committed meanwhile');

sql(q{SELECT dolt_commit('-am', 'later')});
is(sql(q{SELECT count(*) FROM lu.k WHERE v = 'rolled back'}), '0',
	'what rolled back isn\'t there');
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'and the branch is clean');

$bg->quit;
$bg2->quit;
$node->stop;
done_testing();
