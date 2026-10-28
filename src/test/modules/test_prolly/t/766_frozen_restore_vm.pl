# Copyright (c) 2026, PostgreSQL Global Development Group

# A restore of a frozen root, the relation changed since, gives it a
# visibility map: the source's copied, and each changed page's bits
# cleared (its frozen copy not read), each map page logged as an image.
# The pages unchanged since keep their bits; the changed ones have none.
# Redo replays it: after a crash, and on a standby, the map is the same,
# page by page checked (wal_consistency_checking), and true of the pages.
# A plain VACUUM then sets the changed pages' bits again without copying
# them, replayed the same way.
#
# The updates are HOT (fillfactor 50), so the pages changed are those the
# updated rows were on.

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
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('frozen_restore_vm_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $summary = q{SELECT all_visible || ',' || all_frozen FROM pg_visibility_map_summary('fv.t')};
# (the changed pages with a bit set, and the unchanged ones without both)
sub wrong
{
	my $changed = shift;
	return qq{SELECT count(*) FILTER (WHERE blkno = ANY ('{$changed}'::bigint[])
		AND (all_visible OR all_frozen)) || ',' ||
		count(*) FILTER (WHERE NOT blkno = ANY ('{$changed}'::bigint[])
		AND NOT (all_visible AND all_frozen))
		FROM pg_visibility_map('fv.t')};
}
my $clean = q{SELECT (SELECT count(*) FROM pg_check_frozen('fv.t')) +
	(SELECT count(*) FROM pg_check_visible('fv.t'))};
my $pages = q{SELECT pg_relation_size('fv.t') / 8192};

sql(q{CREATE EXTENSION pg_visibility;
	CREATE SCHEMA fv VERSIONED;
	CREATE TABLE fv.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL)
	  WITH (fillfactor = 50);
	INSERT INTO fv.t SELECT g, 0, repeat('w', 40) FROM generate_series(1, 5000) g;
	VACUUM (FREEZE) fv.t;
	SELECT dolt_commit('-Am', 'c1')});
sql('CHECKPOINT');
my $n = sql($pages);
is(sql($summary), "$n,$n", 'before: every page all-visible and all-frozen');
my $changed = sql(q{SELECT string_agg(DISTINCT ((ctid::text::point)[0])::bigint::text, ',')
	FROM fv.t WHERE id <= 500});
my $nchanged = scalar(split /,/, $changed);
ok($nchanged > 0 && $nchanged < $n, "some pages changed ($nchanged of $n)");
sql(q{UPDATE fv.t SET v = 1 WHERE id <= 500; SELECT dolt_commit('-am', 'c2')});
sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(sql($pages), $n, 'the restored relation is the root\'s length');
my $kept = $n - $nchanged;
is(sql($summary), "$kept,$kept",
	'the restored relation\'s map: the unchanged pages all-visible and all-frozen');
is(sql(wrong($changed)), '0,0',
	'the changed pages cleared, the unchanged ones kept');
is(sql(q{SELECT count(*) FROM fv.t WHERE v = 0}), '5000', 'the rows are the commit\'s');

# a crash: recovery replays the map
$node->stop('immediate');
$node->start;
is(sql($summary), "$kept,$kept", 'after a crash, the map as it was');
is(sql(wrong($changed)), '0,0', 'page by page');
is(sql($clean), '0', 'and true of the pages');

$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $summary), "$kept,$kept",
	'the standby replayed the map');
is($standby->safe_psql('postgres', wrong($changed)), '0,0', 'page by page');
is($standby->safe_psql('postgres', $clean), '0', 'true of its pages too');

# A plain VACUUM sets the changed pages' bits again: they are the root's
# still, shared with it, so it leaves them as they are, but they say they
# are all-visible and hold only frozen versions.  An index-only scan then
# fetches no row from the heap.
my $fetches = q{SET enable_seqscan = off; SET enable_bitmapscan = off;
	EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
	SELECT count(id) FROM fv.t WHERE id <= 5000};
sub heap_fetches
{
	my ($out) = @_;
	return $out =~ /Heap Fetches: (\d+)/ ? $1 : 'none';
}
ok(heap_fetches(sql($fetches)) > 0, 'before VACUUM, an index-only scan fetches from the heap');
my ($vstdout, $verbose);
$node->psql('postgres', 'VACUUM (VERBOSE) fv.t', stdout => \$vstdout, stderr => \$verbose);
my ($left) = $verbose =~ /pages shared: (\d+) shared/;
ok(defined $left && $left >= $nchanged,
	'VACUUM left the shared pages as they are (' . ($left // 'none') . ')');
is(sql($summary), "$n,$n", 'after VACUUM, every page all-visible and all-frozen');
is(heap_fetches(sql($fetches)), '0', 'an index-only scan fetches no row from the heap');
is(sql($clean), '0', 'the map true of the pages');

$node->stop('immediate');
$node->start;
is(sql($summary), "$n,$n", 'after a crash, VACUUM\'s bits as they were');
is(heap_fetches(sql($fetches)), '0', 'still no heap fetch');
is(sql($clean), '0', 'and true of the pages');

$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $summary), "$n,$n",
	'the standby replayed VACUUM\'s bits');
is(heap_fetches($standby->safe_psql('postgres', $fetches)), '0',
	'no heap fetch on the standby');
is($standby->safe_psql('postgres', $clean), '0', 'true of its pages');

$standby->stop;
$node->stop;
done_testing();
