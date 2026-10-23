# Copyright (c) 2026, PostgreSQL Global Development Group

# Snapshot slots replayed over a newer root page.  A root page's image
# restored over a newer version of it on disk keeps the newer page's later
# subtree pointers, and with them its LSN, so the records in between are
# not replayed on it: the newer page's snapshot slots are kept too.  A
# snapshot taken after the image is not lost, and one released after it
# does not come back (its subtrees, already let go of, would be let go of
# a second time).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('bmap_snapshot_redo');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
shared_buffers = 16MB
checkpoint_timeout = 1h
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION pg_buffercache');

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub rows
{
	my ($rel) = @_;
	return sql(qq{SELECT test_bmap_rows_digest($rel)});
}

sub snapshots
{
	my ($rel) = @_;
	return sql(qq{SELECT coalesce(string_agg(id::text, ' ' ORDER BY id), '')
		FROM test_bmap_snapshots($rel)});
}

sub sweep_frees
{
	return sql(q{SELECT freed FROM prolly_page_sweep()});
}

# the root page written out as of the last change, then a crash: its
# version on disk is newer than the image replay starts from
sub evict_map_and_crash
{
	my ($rel) = @_;
	sql(qq{SELECT count(*) FROM pg_buffercache
		WHERE reldatabase = (SELECT oid FROM pg_database
							 WHERE datname = current_database())
		AND relfilenode = $rel AND relforknumber = 4
		AND (pg_buffercache_evict(bufferid)).buffer_evicted});
	$node->stop('immediate');
	$node->start;
}

sql(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 10);
	INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 2000) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});

# a snapshot taken after the root page's image, before a path copy that
# changes a pointer
sql('CHECKPOINT');
my $s1 = sql(qq{SELECT test_bmap_snapshot_take($rel)});
my $s2 = sql(qq{SELECT test_bmap_snapshot_take($rel)});
sql(q{UPDATE t SET b = 'y' WHERE a = 1});
my $now = rows($rel);
evict_map_and_crash($rel);
is(snapshots($rel), "$s1 $s2", 'a snapshot taken after the image is kept');
is(rows($rel), $now, 'and the table recovers');
is(sweep_frees(), '0', 'a sweep finds nothing held by nothing');

# a snapshot released after the root page's image, before a path copy
ok(sql(qq{SELECT test_bmap_snapshot_release($rel, $s1)}) eq 't'
	  && sql(qq{SELECT test_bmap_snapshot_release($rel, $s2)}) eq 't',
	'the snapshots are let go of');
my $s4 = sql(qq{SELECT test_bmap_snapshot_take($rel)});
sql('CHECKPOINT');
my $s3 = sql(qq{SELECT test_bmap_snapshot_take($rel)});
is(sql(qq{SELECT test_bmap_snapshot_release($rel, $s4)}), 't',
	'a snapshot is released after the image');
sql(q{UPDATE t SET b = 'z' WHERE a = 2});
$now = rows($rel);
evict_map_and_crash($rel);
is(snapshots($rel), "$s3", 'a snapshot released after the image stays released');
is(sql(qq{SELECT test_bmap_snapshot_release($rel, $s4)}), 'f',
	'and is not released a second time');
is(rows($rel), $now, 'the table recovers');
is(sweep_frees(), '0', 'a sweep finds nothing held by nothing');

# with everything let go of, what was held is freed
is(sql(qq{SELECT test_bmap_snapshot_release($rel, $s3)}), 't',
	'the last snapshot is let go of');
sql(q{DROP TABLE t});
is(sweep_frees(), '0', 'dropping the table freed all it held');

$node->stop;
done_testing();
