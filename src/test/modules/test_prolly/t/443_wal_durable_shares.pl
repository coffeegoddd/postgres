# Copyright (c) 2026, PostgreSQL Global Development Group

# At a WAL level for backups, a share or a kept snapshot of block-mapped
# storage is made durable by WAL: each block it shares that was its own is
# logged as an image first, and its write is synced by the next checkpoint,
# as any page's.  So it doesn't sync the database's page store (#947), and
# its record says the images were logged.  A write the machine lost (the
# block zeroed, as an unsynced write may be after a crash of the machine)
# is rebuilt by recovery from the image, even once the server comes back at
# wal_level minimal; after a checkpoint the block was synced by it.  At
# minimal, the store is synced as before.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('wal_durable_shares');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf',
	"autovacuum = off\ncheckpoint_timeout = 1h\nwal_compression = off\n"
	  . "prolly.restore_min_changes = 0");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_walinspect;
	CREATE EXTENSION pg_buffercache;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text);
	INSERT INTO app.t SELECT i, 0, repeat('x', 300) FROM generate_series(1, 4000) i;
	SELECT dolt_commit('-Am', 'base');
});

# The page-store syncs a session's statement 'op' ran, after 'setup'.
sub syncs_of
{
	my ($setup, $op) = @_;
	my $out = sql(qq{
		$setup;
		CREATE TEMP TABLE before AS SELECT * FROM test_prolly_session_stats();
		$op;
		SELECT 'syncs=' || (s.value - b.value)
		FROM test_prolly_session_stats() s JOIN before b USING (name)
		WHERE s.name = 'bmap_store_syncs';
	});
	return ($out =~ /^syncs=(\d+)$/m)[0];
}

my $contents = q{SELECT count(*), sum(v), sum(length(pad)) FROM app.t};
my $store = $node->data_dir . '/base/'
  . sql(q{SELECT oid FROM pg_database WHERE datname = current_database()})
  . '/9421';

# Zero physical block 'phys' of the page store, as a write the machine
# lost leaves it.
sub lose_block
{
	my ($phys) = @_;
	my $seg = int($phys / 131072);
	my $path = $seg ? "$store.$seg" : $store;
	open(my $fh, '+<:raw', $path) or die "open $path: $!";
	seek($fh, ($phys % 131072) * 8192, 0) or die "seek $path: $!";
	print $fh ("\0" x 8192);
	close($fh) or die "close $path: $!";
}

# The physical blocks of app.t's heap pages holding the rows 'where'.
sub blocks_of
{
	my ($where) = @_;
	return split /\n/, sql(qq{
		SELECT DISTINCT m.phys FROM app.t t,
			test_bmap_map(pg_relation_filenode('app.t')) m
		WHERE $where AND m.blkno = (t.ctid::text::point)[0]});
}

# A commit keeps snapshots holding blocks of their own: no sync, and the
# records say their images were logged.
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
is( syncs_of(
		q{UPDATE app.t SET v = v + 1 WHERE id % 10 = 0},
		q{SELECT dolt_commit('-am', 'c1')}),
	'0', 'a commit keeping pages changed since syncs no page store');
my $end = sql(q{SELECT pg_current_wal_insert_lsn()});
ok( sql(qq{SELECT count(*) > 0 AND bool_and(description LIKE '%(images logged)%')
		FROM pg_get_wal_records_info('$start', '$end')
		WHERE resource_manager = 'BlockMap' AND description LIKE '%snapshot%'}) eq 't',
	'each kept snapshot\'s record says its own blocks\' images were logged');
ok( sql(qq{SELECT count(*) > 0 FROM pg_get_wal_records_info('$start', '$end')
		WHERE resource_manager = 'BlockMap' AND record_type = 'PRESERVE'}) eq 't',
	'and the images are there');

# A share of a relation with blocks of its own: none either.
sql(q{SET debug_block_mapped_heap = on;
	CREATE TABLE plain (a int, b text);
	INSERT INTO plain SELECT g, repeat('p', 200) FROM generate_series(1, 2000) g});
my $plain = sql(q{SELECT pg_relation_filenode('plain')});
is(syncs_of(q{UPDATE plain SET a = a + 0 WHERE a % 10 = 0},
		qq{SELECT test_bmap_share($plain)}),
	'0', 'a share of pages its own syncs no page store');
sql(q{UPDATE plain SET a = a + 0 WHERE a % 10 = 2});
my $digest = sql(qq{SELECT test_bmap_rows_digest($plain)});
$start = sql(q{SELECT pg_current_wal_insert_lsn()});
my $copy = sql(qq{SELECT test_bmap_share($plain)});
$end = sql(q{SELECT pg_current_wal_insert_lsn()});
is(sql(qq{SELECT count(*) FROM pg_get_wal_records_info('$start', '$end')
		WHERE record_type = 'SHARED' AND description LIKE '%(images logged)%'}),
	'1', 'the share\'s record says the images were logged');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'the share reads the source\'s rows');
is(syncs_of(q{UPDATE plain SET a = a + 0 WHERE a % 10 = 1},
		qq{SELECT test_bmap_snapshot_take($plain)}),
	'0', 'nor does a snapshot taken on its own');

# A commit's kept blocks lost by the machine, after the working set moved
# off them (its pages changed again, copied first): recovery rebuilds them
# from the images, the checkpoint before them being where it begins, and a
# hard reset back to the commit gives them back.
sql(q{UPDATE app.t SET v = v + 1, pad = pad || 'z' WHERE id % 7 = 0;
	SELECT dolt_commit('-am', 'c2')});
my $kept = sql($contents);
my @lost = blocks_of('t.id % 7 = 0');
ok(@lost > 0, 'the commit changed pages: ' . scalar(@lost));
sql(q{UPDATE app.t SET v = v + 1 WHERE id % 7 = 0});
my $moved = sql($contents);
is(scalar(grep { my $b = $_; grep { $_ == $b } @lost } blocks_of('t.id % 7 = 0')),
	0, 'the working set moved off the kept blocks');
# (its map written out: recovery then skips the records of the pages from
# before they moved, as of a newer map, so only the images rebuild them)
is(sql(q{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted)
	FROM pg_buffercache WHERE relfilenode = pg_relation_filenode('app.t')}),
	't', 'the table\'s buffers written out and evicted');
$node->stop('immediate');
lose_block($_) for @lost;
$node->start;
is(sql($contents), $moved, 'after a crash, the working set');
my $restored = sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'relations_restored'});
is(sql(q{SELECT dolt_reset('--hard'); SELECT pg_stat_force_next_flush();
	SELECT count(*), sum(v), sum(length(pad)) FROM app.t}) =~ s/^.*\n//sr,
	$kept, 'after their writes were lost, a hard reset gives back the commit\'s pages');
cmp_ok(sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'relations_restored'}),
	'>', $restored, 'restored from the kept blocks');

# After a checkpoint, the same commit's blocks are on disk: a crash then
# needs no image.
sql(q{UPDATE app.t SET v = v + 1 WHERE id % 11 = 0;
	SELECT dolt_commit('-am', 'c3'); CHECKPOINT});
my $c3 = sql($contents);
$node->stop('immediate');
$node->start;
is(sql($contents), $c3, 'after a checkpoint and a crash, the commit\'s pages');

# Logged at replica, recovered at minimal: redo goes by the record.
sql(q{UPDATE app.t SET v = v + 2 WHERE id % 13 = 0;
	SELECT dolt_commit('-am', 'c4')});
my $c4 = sql($contents);
@lost = blocks_of('t.id % 13 = 0');
$node->stop('immediate');
lose_block($_) for @lost;
$node->append_conf('postgresql.conf',
	"wal_level = minimal\nmax_wal_senders = 0\nmax_replication_slots = 0");
$node->start;
is(sql($contents), $c4,
	'WAL written at replica rebuilds the lost pages at wal_level minimal');
is( syncs_of(
		q{UPDATE app.t SET v = v + 1 WHERE id % 10 = 0},
		q{SELECT dolt_commit('-am', 'c5')}),
	'1', 'at minimal, a commit keeping pages syncs the page store');
my $c5 = sql($contents);
$node->stop('immediate');
$node->start;
is(sql($contents), $c5, 'and its pages survive a crash at minimal');

$node->stop;
done_testing();
