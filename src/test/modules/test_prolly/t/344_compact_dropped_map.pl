# Copyright (c) 2026, PostgreSQL Global Development Group

# A page sweep's compaction moves no page of a relation whose drop is in
# flight: committed, its pages' buffers dropped, its map about to be let
# go of and its blocks freed.  Moving them would bring its pages back into
# shared buffers, dirty, for blocks about to be freed.  As the drop goes
# on, a checkpoint writes nothing of it, and the page store stays whole.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('compact_dropped_map');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# y first, then t and x extended in turn, so that x's blocks, freed, leave
# holes among t's for a compaction to move t's pages down into
sql(q{
	CREATE EXTENSION injection_points;
	CREATE EXTENSION test_prolly;
	CREATE TABLE y (a int PRIMARY KEY, b text);
	INSERT INTO y SELECT g, repeat('y', 300) FROM generate_series(1, 400) g;
	CREATE TABLE t (a int PRIMARY KEY, b text) WITH (fillfactor = 10);
	CREATE TABLE x (a int, b text) WITH (fillfactor = 10);
});
sql(q{DO $$
	BEGIN
		FOR i IN 1 .. 200 LOOP
			INSERT INTO t SELECT g, repeat('t', 300) FROM generate_series(i * 10, i * 10 + 9) g;
			INSERT INTO x SELECT g, repeat('x', 300) FROM generate_series(i * 10, i * 10 + 9) g;
		END LOOP;
	END $$});
sql(q{CHECKPOINT; DROP TABLE x});

# x's blocks let go of (the release queue is drained from its top, which
# t's drop, in flight, would hold up)
sql(q{SELECT test_bmap_release_drain()});
cmp_ok(sql(q{SELECT test_bmap_free_blocks()}), '>', 0, 'x\'s blocks are free');

# t dropped: committed, its pages' buffers dropped, its map not yet let go
my $drop = $node->background_psql('postgres');
$drop->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-predrop', 'wait');
	SELECT injection_points_attach('bmap-dropped', 'wait')});
$drop->query_until(qr/started/, q{
	\echo started
	DROP TABLE t;
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-predrop'}) or die 'the drop did not stop';

# a sweep meanwhile frees x's blocks and compacts the store: none of t's
# pages moves (y's are below x's, t's are the only ones above)
is(sql(q{SELECT moved FROM prolly_page_sweep()}), '0', 'no page of the dropped table moves');

# the drop lets go of t's map; a checkpoint meanwhile writes nothing of t
sql(q{SELECT injection_points_detach('bmap-predrop')});
sql(q{SELECT injection_points_wakeup('bmap-predrop')});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-dropped'}) or die 'the drop did not let go of the map';
my ($ret, $out, $err) = $node->psql('postgres', 'CHECKPOINT');
is($ret, 0, 'a checkpoint as the map is let go of');
is($err, '', 'writes no page of it');
sql(q{SELECT injection_points_detach('bmap-dropped')});
sql(q{SELECT injection_points_wakeup('bmap-dropped')});
$drop->query_until(qr/done/, '');
$drop->quit;

# the store whole: a sweep after frees t's blocks
sql(q{SELECT count(*) FROM prolly_page_sweep()});
is(sql(q{SELECT count(*) FROM y WHERE b = repeat('y', 300)}), '400', 'y reads');
$node->restart;
is(sql(q{SELECT count(*) FROM y WHERE b = repeat('y', 300)}), '400', 'and after a restart');
is(sql(q{SELECT count(*) FROM prolly_page_sweep() WHERE freed = 0}), '1',
	'nothing left for a sweep to free');

$node->stop;
done_testing();
