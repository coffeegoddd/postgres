# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum drains the release queue in the background: a dropped table's
# queued storage is let go of by the workers' release step, at most
# prolly.release_max_per_pass entries a pass, so a queue larger than that
# takes passes, and every block comes back.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('bmap_release_autovacuum');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
autovacuum_naptime = 1
prolly.release_max_per_pass = 300
log_min_messages = debug1
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int, b int8) WITH (fillfactor = 10, autovacuum_enabled = off);
	INSERT INTO t SELECT g, g FROM generate_series(1, 20000) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
my $blocks = sql(q{SELECT pg_relation_size('t') / 8192 +
	(SELECT count(*) FROM test_bmap_past_end(pg_relation_filenode('t')))});
my $nodes = sql(qq{SELECT count(DISTINCT block) FROM test_bmap_tree($rel)});
cmp_ok($blocks, '>', 1000, 'the table has more blocks than a pass lets go of');
# (the sweep due after a start, run now: autovacuum's would drain the
# queue itself, at once)
sql(q{SELECT * FROM prolly_page_sweep()});
is(sql(q{SELECT test_bmap_sweep_due()}), 'f', 'no sweep is due');
my $free = sql(q{SELECT test_bmap_free_blocks()});
sql('DROP TABLE t');
cmp_ok(sql(q{SELECT test_bmap_release_pending()}), '>', 0, 'the drop queues');

# autovacuum on: its workers drain the queue
my $log_offset = -s $node->logfile;
$node->append_conf('postgresql.conf', 'autovacuum = on');
$node->reload;
ok( $node->poll_query_until(
		'postgres', q{SELECT test_bmap_release_pending() = 0}),
	'autovacuum drains the queue');
is(sql(q{SELECT test_bmap_free_blocks()}) - $free, $blocks + $nodes,
	'and every block of the table is freed');

# in passes of at most prolly.release_max_per_pass entries, each counted
my $log = slurp_file($node->logfile, $log_offset);
my @passes = ($log =~ /automatic release of database "postgres": (\d+) entries/g);
my @drained = grep { $_ > 0 } @passes;
cmp_ok(scalar(@drained), '>=', 2, 'over more than one pass');
ok(!grep({ $_ > 300 + 128 } @drained),
	'each letting go of no more than the bound and one step past it');
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->reload;
ok( $node->poll_query_until(
		'postgres', q{SELECT value > 0 FROM pg_stat_prolly
			WHERE name = 'bmap_release_drained'}),
	'pg_stat_prolly counts the drained entries');

$node->stop;
done_testing();
