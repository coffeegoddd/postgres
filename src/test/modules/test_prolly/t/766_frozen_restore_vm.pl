# Copyright (c) 2026, PostgreSQL Global Development Group

# A restore of a frozen root, the relation changed since, gives it a
# visibility map: the source's copied, and each changed page's bits set
# from its frozen copy, each map page logged as an image.  Redo replays
# it: after a crash, and on a standby, the map is the same, page by page
# checked (wal_consistency_checking), and true of the pages.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('frozen_restore_vm');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_consistency_checking = 'all'
prolly.restore_min_changes = 0
prolly.kept_commits = 8
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('frozen_restore_vm_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $summary = q{SELECT all_visible || ',' || all_frozen FROM pg_visibility_map_summary('fv.t')};
my $clean = q{SELECT (SELECT count(*) FROM pg_check_frozen('fv.t')) +
	(SELECT count(*) FROM pg_check_visible('fv.t'))};
my $pages = q{SELECT pg_relation_size('fv.t') / 8192};

sql(q{CREATE EXTENSION pg_visibility;
	CREATE SCHEMA fv VERSIONED;
	CREATE TABLE fv.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL);
	INSERT INTO fv.t SELECT g, 0, repeat('w', 40) FROM generate_series(1, 5000) g;
	VACUUM fv.t;
	SELECT dolt_commit('-Am', 'c1')});
sql('CHECKPOINT');
sql(q{UPDATE fv.t SET v = 1; SELECT dolt_commit('-am', 'c2')});
sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
my $n = sql($pages);
is(sql($summary), "$n,$n", 'the restored relation\'s map: every page all-visible and all-frozen');

# a crash: recovery replays the map
$node->stop('immediate');
$node->start;
is(sql($summary), "$n,$n", 'after a crash, the map as it was');
is(sql($clean), '0', 'and true of the pages');

$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $summary), "$n,$n",
	'the standby replayed the map');
is($standby->safe_psql('postgres', $clean), '0', 'true of its pages too');

$standby->stop;
$node->stop;
done_testing();
