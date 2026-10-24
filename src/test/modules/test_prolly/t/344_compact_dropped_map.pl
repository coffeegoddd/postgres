# Copyright (c) 2026, PostgreSQL Global Development Group

# A page sweep's compaction moves no page of a relation whose drop is in
# flight: committed, its pages' buffers dropped, its map about to be let
# go of.  A move then would be logged after the drop's commit record, whose
# replay removes the map: a standby replaying it would find no map, and
# fail there at every restart.  So the standby replays past the drop, and
# the page store stays whole.  And a relation's moves stop once another
# waits for compaction's hold on it: a DDL waits for one move, not all.

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
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('compact_dropped_map_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

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
sql(q{CREATE TABLE marker (a int)});
ok($standby->poll_query_until('postgres', q{SELECT to_regclass('marker') IS NOT NULL}),
	'a standby replays past the drop');
$standby->restart;
is($standby->safe_psql('postgres', q{SELECT count(*) FROM y}), '400', 'and after a restart');

# the store whole: a sweep after frees t's blocks
sql(q{SELECT count(*) FROM prolly_page_sweep()});
is(sql(q{SELECT count(*) FROM y WHERE b = repeat('y', 300)}), '400', 'y reads');
$node->restart;
is(sql(q{SELECT count(*) FROM y WHERE b = repeat('y', 300)}), '400', 'and after a restart');
is(sql(q{SELECT count(*) FROM prolly_page_sweep() WHERE freed = 0}), '1',
	'nothing left for a sweep to free');

# a relation's moves stop once a DDL waits for compaction's hold on it
sql(q{
	CREATE TABLE t2 (a int, b text) WITH (fillfactor = 10);
	CREATE TABLE x2 (a int, b text) WITH (fillfactor = 10);
});
sql(q{DO $$
	BEGIN
		FOR i IN 1 .. 200 LOOP
			INSERT INTO t2 SELECT g, repeat('t', 300) FROM generate_series(i * 10, i * 10 + 9) g;
			INSERT INTO x2 SELECT g, repeat('x', 300) FROM generate_series(i * 10, i * 10 + 9) g;
		END LOOP;
	END $$});
sql(q{DROP TABLE x2});
sql(q{SELECT test_bmap_release_drain()});
my $sweep = $node->background_psql('postgres');
my $sweeper = $sweep->query_safe(q{SELECT pg_backend_pid()});
$sweep->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-compact-room', 'wait')});
$sweep->query_until(qr/started/, q{
	\echo started
	SELECT 'moved ' || moved FROM prolly_page_sweep();
	\echo done
});

# stopped before a move, holding t2 (whose pages, past x2's freed ones,
# are the only ones to move: it has no index)
$node->poll_query_until('postgres', qq{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE pid = $sweeper AND wait_event = 'bmap-compact-room'})
  or die 'the compaction did not stop';
is( sql(qq{SELECT count(*) FROM pg_locks
	WHERE pid = $sweeper AND locktype = 'relation'
	  AND relation = 't2'::regclass AND granted}),
	'1', 'the compaction stopped holding t2');

my $ddl = $node->background_psql('postgres');
$ddl->query_until(qr/started/, q{
	\echo started
	ALTER TABLE t2 ADD COLUMN c int;
	\echo done
});
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_locks
		  WHERE locktype = 'relation' AND relation = 't2'::regclass
		    AND mode = 'AccessExclusiveLock' AND NOT granted}),
	'a DDL waits for the compaction');
sql(q{SELECT injection_points_detach('bmap-compact-room')});
sql(q{SELECT injection_points_wakeup('bmap-compact-room')});
my $swept = $sweep->query_until(qr/done/, '');
$ddl->query_until(qr/done/, '');
my ($nmoved) = $swept =~ /moved (\d+)/;
cmp_ok($nmoved, '<=', 2, 'which stops its moves of t2 after about one (of some 500)');
$sweep->quit;
$ddl->quit;
cmp_ok(sql(q{SELECT moved FROM prolly_page_sweep()}), '>', 0, 'a later sweep moves the rest');
is(sql(q{SELECT count(*) FROM t2}), '2000', 't2 reads');

$node->stop;
$standby->stop;
done_testing();
