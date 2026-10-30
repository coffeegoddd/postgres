# Copyright (c) 2026, PostgreSQL Global Development Group

# Block-mapped pages evicted under pressure (few buffers): a change that
# copies a page shared with a copy takes a new block and changes reference
# counts, and the buffers that needs may evict a dirty page of the same
# relation whose entry is on the map page of the page it changes.  Writing
# that page locks the map page to look it up, so the change locks the map
# page only after it has read those buffers.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('evictions');
# (WAL of a level above minimal: there, CREATE DATABASE needs more buffers
# than these)
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
shared_buffers = 16
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE ev');
$node->safe_psql('ev', q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int8 PRIMARY KEY, b int8);
	INSERT INTO t SELECT g, g FROM generate_series(1, 3000) g;
});
$node->safe_psql('ev', 'CHECKPOINT');

sub sql { return $node->safe_psql('ev', $_[0]); }

my $rel = sql(q{SELECT pg_relation_filenode('t')});
is(sql(q{SELECT count(*) || ' ' || sum(b) FROM t}), '3000 4501500', 'read');
my $copy = sql(qq{SELECT test_bmap_share($rel)});
my $digest = sql(qq{SELECT test_bmap_rows_digest($copy)});
sql(q{UPDATE t SET b = b + 1 WHERE a % 100 = 0});
is(sql(q{SELECT count(*) || ' ' || sum(b) FROM t}), '3000 4501530',
	'a change evicting a page under its own map page goes through');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'and leaves the copy as it was');
sql(qq{SELECT test_bmap_unlink($copy)});

$node->stop;
done_testing();
