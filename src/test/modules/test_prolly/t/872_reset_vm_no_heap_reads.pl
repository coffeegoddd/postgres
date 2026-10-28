
# Copyright (c) 2026, PostgreSQL Global Development Group

# A hard reset to a commit kept with a frozen root gives each relation
# changed since new storage of the root, with a copy of its visibility map
# whose bits are cleared for the pages changed since.  It reads none of
# those pages for that: the relation pages the reset's backend reads or
# finds in its buffers (pg_stat_io) don't grow with the working set it
# discards, 90% of the table's rows changed against 10%.  A VACUUM after
# it sets the bits again with a few bytes of WAL a page.
#
# The updates are HOT (fillfactor 50), so the table's own pages are the
# ones changed.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('reset_vm_no_heap_reads');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
fsync = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $nrows = 30000;
sql(q{ALTER DATABASE postgres SET prolly.root_interval = 0;
	ALTER DATABASE postgres SET prolly.root_interval_commits = 1});
sql(qq{CREATE EXTENSION pg_visibility;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, rev int NOT NULL, payload text NOT NULL)
	  WITH (fillfactor = 50);
	INSERT INTO app.t SELECT g, 0, repeat(md5(g::text), 12)
	  FROM generate_series(1, $nrows) g;
	VACUUM (FREEZE) app.t;
	SELECT dolt_commit('-Am', 'load') IS NOT NULL});
my $head = sql(q{SELECT dolt_hashof('HEAD')});
my $blocks = sql(q{SELECT pg_relation_size('app.t') / 8192});
ok($blocks > 1000, "the table has many pages ($blocks)");

my $io = q{SELECT coalesce(sum(coalesce(reads, 0) + coalesce(hits, 0)), 0)
	FROM pg_stat_io WHERE backend_type = 'client backend' AND object = 'relation'};

# the first $pct% of the rows updated, then a hard reset to the head's
# commit: the relation pages it read or hit, and the table's pages its
# map leaves without bits
sub dirty_reset
{
	my ($pct) = @_;

	sql(qq{UPDATE app.t SET rev = rev + 1 WHERE id <= $nrows * $pct / 100;
		SELECT pg_stat_force_next_flush()});
	my $before = sql($io);
	sql(qq{SELECT dolt_reset('--hard', '$head') IS NOT NULL;
		SELECT pg_stat_force_next_flush()});
	my $after = sql($io);
	is(sql(q{SELECT count(*) FROM app.t WHERE rev <> 0}), '0',
		"$pct% dirty: the rows are the commit's");
	my $cleared = sql(q{SELECT count(*) FROM pg_visibility_map('app.t')
		WHERE NOT (all_visible AND all_frozen)});
	return ($after - $before, $cleared);
}

my ($io10, $cleared10) = dirty_reset(10);
my ($io90, $cleared90) = dirty_reset(90);
note("relation pages read or hit: at 10% $io10, at 90% $io90; "
	  . "pages cleared: $cleared10, $cleared90");

# (the map cleared for the pages changed: a tenth of them, then nine)
ok($cleared10 >= $blocks / 10 - 2 && $cleared10 < $blocks / 5,
	"10% dirty: the changed pages cleared ($cleared10)");
ok($cleared90 >= $blocks * 9 / 10 - 2,
	"90% dirty: the changed pages cleared ($cleared90)");

# (a frozen copy read for each of the 80% more pages changed would add
# about 0.8 * $blocks)
ok($io90 - $io10 < $blocks / 20,
	"no heap page read for the pages changed (grew by " . ($io90 - $io10) . ")");

# A VACUUM then sets the cleared pages' bits again, leaving the pages (the
# root's still, shared with it) as they are.  Each map change is logged as
# a record of the map page alone, a few bytes but for the map page's first
# image after a checkpoint, not an image a page: the WAL it writes is
# far below the table's size.
my $lsn0 = sql('SELECT pg_current_wal_insert_lsn()');
sql('VACUUM app.t');
my $wal = sql(qq{SELECT pg_current_wal_insert_lsn() - '$lsn0'::pg_lsn});
note("VACUUM after the reset wrote $wal bytes of WAL for $blocks pages");
is(sql(q{SELECT count(*) FROM pg_visibility_map('app.t')
	WHERE NOT (all_visible AND all_frozen)}), '0',
	'the VACUUM set every page all-visible and all-frozen');
ok($wal < $blocks * 512,
	"its WAL far below the table's size ($wal bytes, $blocks pages)");

$node->stop;
done_testing();
