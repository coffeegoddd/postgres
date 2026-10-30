# Copyright (c) 2026, PostgreSQL Global Development Group

# A root captured of a block-mapped table keeps naming the table's storage
# after the table is dropped, while it is held.  The table's OID and file
# number may then be given to a new table (the OID counter wraps around):
# the new table must not be taken for the one the root was captured of.
# Its changes must not reach the root's pages, a sweep must count the
# root's blocks and the new table's apart, and letting go of the root must
# free only what the root held.  The OID counter is set back to the
# dropped table's file number, as a wraparound would bring it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('root_reuse');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, 'old ' || g FROM generate_series(1, 3000) g;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
is(sql(q{SELECT oid FROM pg_class WHERE relname = 't'}), $t,
	'the table\'s OID is its file number');
my $root = sql(qq{SELECT test_bmap_root_capture($t)});
my $digest = sql(qq{SELECT test_bmap_rows_digest($t)});
my $t_blocks = sql(qq{SELECT count(*) FROM test_bmap_map($t)});

# The table dropped, its storage let go of (the root still holds its
# blocks), and its files removed by the checkpoints after (an empty main
# fork keeps the file number until the checkpoint after the drop's).
sql(q{DROP TABLE t});
sql(q{SELECT * FROM test_bmap_release_drain()});
sql(q{CHECKPOINT});
sql(q{CHECKPOINT});
# (and its catalog rows vacuumed away: a new OID is checked against dead
# rows too)
sql(q{VACUUM pg_class});
my $t_path = $node->data_dir . '/' . "base/" . sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'}) . "/$t";
ok(!-e $t_path && !-e "${t_path}_bmap", 'the dropped table\'s files are gone');
is(sql(qq{SELECT test_bmap_exists($t)}), 'f', 'the dropped table\'s storage is gone');
is(sql(qq{SELECT test_bmap_root_held($root) > 0}), 't', 'and the root is held');

$node->stop;
command_ok([ 'pg_resetwal', '-o', $t, $node->data_dir ],
	'the OID counter is set back to the dropped table\'s file number');
$node->start;

sql(q{
	CREATE TABLE u (a int, b text);
	INSERT INTO u SELECT g, 'new ' || g FROM generate_series(1, 2000) g;
});
my $u = sql(q{SELECT pg_relation_filenode('u')});
is($u, $t, 'the new table has the dropped table\'s file number');

# The new table's changes don't reach the root's pages, and a sweep
# frees none of the root's blocks nor the new table's.
sql(q{UPDATE u SET b = b || ' changed'});
my $free = sql(q{SELECT test_bmap_free_blocks()});
sql(q{SELECT * FROM test_bmap_sweep()});
my $copy = sql(qq{SELECT test_bmap_storage_from_root($root)});
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'storage made of the root reads the dropped table\'s rows');
is(sql(q{SELECT count(*) FROM u WHERE b LIKE 'new % changed'}), '2000',
	'the new table reads its own');
# (Storage dropped by the helper has no commit record to remove its map in
# redo: a checkpoint keeps crash recovery from making it again.)
sql(qq{SELECT test_bmap_unlink($copy)});
sql(q{CHECKPOINT});

# Letting go of the root frees the dropped table's blocks, and nothing of
# the new table's, after a crash too.
$free = sql(q{SELECT test_bmap_free_blocks()});
sql(qq{SELECT test_bmap_root_release($root)});
sql(q{SELECT * FROM test_bmap_release_drain()});
cmp_ok(sql(q{SELECT test_bmap_free_blocks()}) - $free, '>=', $t_blocks,
	'letting go of the root frees the dropped table\'s blocks');
sql(q{SELECT * FROM test_bmap_sweep()});
is(sql(q{SELECT count(*) FROM u WHERE b LIKE 'new % changed'}), '2000',
	'and leaves the new table\'s');
sql(q{CREATE TABLE flushed ()});
$node->stop('immediate');
$node->start;
is(sql(q{SELECT count(*) FROM u WHERE b LIKE 'new % changed'}), '2000',
	'which a crash keeps');
is(sql(q{SELECT count(*) FROM test_bmap_sweep() WHERE freed = 0}), '1',
	'and a sweep after it finds nothing more to free');

$node->stop;
done_testing();
