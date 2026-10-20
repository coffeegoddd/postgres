# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's copy of a table gets copies of its visibility map and free
# space map, as full-page images in the WAL: after a crash right after the
# branch's first use, recovery replays them, and the branch's index-only
# scans still visit no heap page of an all-visible source, its rows intact.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_maps_crash');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\ncheckpoint_timeout = 1h");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA bm VERSIONED;
	CREATE TABLE bm.t (id int PRIMARY KEY, v text);
	INSERT INTO bm.t SELECT g, 'v' || g FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'loaded')});
sql(q{VACUUM (FREEZE) bm.t});
sql(q{CHECKPOINT});
sql(q{SELECT dolt_branch('other')});

my $fetches = q{SET enable_seqscan = off; SET enable_bitmapscan = off;
	EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
	SELECT count(id) FROM bm.t WHERE id > 0};
my $branch = q{SELECT dolt_checkout('other');};

# the branch's first use shares the table, its maps copied; then a crash
like(sql($branch . $fetches), qr/Heap Fetches: 0/, 'the branch visits no heap page');
$node->stop('immediate');
$node->start;

like(sql($branch . $fetches), qr/Heap Fetches: 0/,
	'after the crash, still none: the maps were replayed');
is(sql(q{SELECT dolt_checkout('other'); SELECT count(*) || '|' || sum(id) FROM bm.t}),
	"{0,\"Switched to branch 'other'\"}\n20000|200010000", 'and its rows are all there');

$node->stop;
done_testing();
