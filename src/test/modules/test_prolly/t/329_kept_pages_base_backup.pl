# Copyright (c) 2026, PostgreSQL Global Development Group

# A base backup copies the page store and the maps at different moments.
# One that copied a kept page's block before the snapshot wrote it out,
# and the relation's map after a later change copied the map path the
# kept pages share, recovers through the newer map, which skips the kept
# pages' records from before the copy (tree_find()).  The images the
# snapshot logged of the pages changed since the last one (PRESERVE),
# written to their blocks in redo, rebuild them.  The fuzzy copy is made
# deterministically, as t/157 does: the block put back as it was before
# the snapshot and the newer map on disk, then recovery.  A base backup
# taken with pg_basebackup afterwards restores the same rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_pages_base_backup');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
wal_consistency_checking = all
shared_buffers = 64MB
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.t};

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_buffercache;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text) WITH (fillfactor = 10);
	INSERT INTO app.t SELECT g, 0, repeat('x', 100) FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'base');
});
sql('CHECKPOINT');

# rows of the first pages changed (copied off the base commit's kept
# pages) and committed, kept in turn; block 0's physical block, as the
# backup copied it, from before the commit wrote it out
sql(q{UPDATE app.t SET v = 1 WHERE id <= 40});
my $rel = sql(q{SELECT pg_relation_filenode('app.t')});
my $phys = sql(qq{SELECT phys FROM test_bmap_map($rel) WHERE blkno = 0});
my $store = $node->data_dir . '/base/'
  . sql(q{SELECT oid FROM pg_database WHERE datname = current_database()})
  . '/9421';
open(my $fh, '<:raw', $store) or die "open $store: $!";
seek($fh, $phys * 8192, 0) or die "seek $store: $!";
my $before;
read($fh, $before, 8192) == 8192 or die "short read of $store";
close($fh) or die "close $store: $!";
sql(q{SELECT dolt_commit('-am', 'first pages')});
my $at = sql($digest);

# then a far page: the path the kept pages share copied
sql(q{UPDATE app.t SET v = -1 WHERE id = 2000});
my $now = sql($digest);
is(sql(qq{SELECT phys FROM test_bmap_map($rel) WHERE blkno = 0}), $phys,
	'block 0 still has the kept physical block');

# the map, as the backup copied it: after the copy of its path
is(sql(q{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted)
	FROM pg_buffercache WHERE relfilenode = pg_relation_filenode('app.t')
		AND relforknumber = 4}),
	't', 'the newer map written out');
$node->stop('immediate');
open($fh, '+<:raw', $store) or die "open $store: $!";
seek($fh, $phys * 8192, 0) or die "seek $store: $!";
print $fh $before or die "write $store: $!";
close($fh) or die "close $store: $!";

$node->start;
is(sql($digest), $now, 'recovered from the fuzzy copy, the working set');
sql(q{SELECT dolt_branch('kept')});
is($node->safe_psql('postgres/kept', $digest), $at,
	"a branch of the head made of the kept pages has the commit's rows");
sql(q{SELECT dolt_reset('--hard')});
is(sql($digest), $at, "the kept pages hold the commit's rows");

# a real base backup, restored
sql(q{UPDATE app.t SET v = 2 WHERE id <= 40});
sql(q{SELECT dolt_commit('-am', 'again')});
my $again = sql($digest);
sql(q{UPDATE app.t SET v = -2 WHERE id = 2000});
$node->backup('bb');
my $restored = PostgreSQL::Test::Cluster->new('kept_pages_restored');
$restored->init_from_backup($node, 'bb');
$restored->start;
is($restored->safe_psql('postgres', $digest), sql($digest),
	'the restored backup has the working set');
$restored->safe_psql('postgres', q{SELECT dolt_reset('--hard')});
is($restored->safe_psql('postgres', $digest), $again,
	"and its kept pages hold the last commit's rows");

$restored->stop;
$node->stop;
done_testing();
