# Copyright (c) 2026, PostgreSQL Global Development Group

# A fold that reads its table in full is logged, with why, when it reads
# at least prolly.log_full_folds rows; not below, nor with -1.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('full_fold_log');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA fl VERSIONED;
	CREATE TABLE fl.t (id int PRIMARY KEY, v text);
	INSERT INTO fl.t SELECT g, 'v' || g FROM generate_series(1, 100) g});

my $at = -s $node->logfile;
sql(q{SET prolly.log_full_folds = 50; SELECT dolt_commit('-Am', 'first')});
ok($node->log_contains(qr/prolly folded table "fl\.t" in full: no_baseline, 100 rows read in \d+ ms/, $at),
	'a full fold past the threshold is logged, with why');

$at = -s $node->logfile;
sql(q{SET prolly.log_full_folds = 500; SET debug_prolly_force_full_fold = on;
	SELECT count(*) FROM dolt_status});
ok(!$node->log_contains(qr/prolly folded table/, $at), 'not one below it');

$at = -s $node->logfile;
sql(q{SET debug_prolly_force_full_fold = on; SELECT count(*) FROM dolt_status});
ok(!$node->log_contains(qr/prolly folded table/, $at), 'nor any by default');

$at = -s $node->logfile;
sql(q{SET prolly.log_full_folds = 0; SET debug_prolly_force_full_fold = on;
	SELECT count(*) FROM dolt_status});
ok($node->log_contains(qr/prolly folded table "fl\.t" in full: forced, 100 rows/, $at),
	'and a forced one says so');

$node->stop;
done_testing();
