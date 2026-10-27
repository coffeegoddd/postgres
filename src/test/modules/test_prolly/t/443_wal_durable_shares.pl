# Copyright (c) 2026, PostgreSQL Global Development Group

# At every WAL level, a share or a kept snapshot of block-mapped storage is
# made durable by WAL: each block it shares that was its own is logged as
# an image first, and its write is synced by the next checkpoint, as any
# page's.  So it doesn't sync the database's page store, and its record
# says the images were logged.  A write the machine lost (the block zeroed,
# as an unsynced write may be after a crash of the machine) is rebuilt by
# recovery from the image, whichever level the WAL was written and is
# replayed at; after a checkpoint the block was synced by it.

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
# (roots sharing the relations' pages as they are: frozen ones share none)
$node->append_conf('postgresql.conf', 'prolly.freeze_roots = off');
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

# The WAL a session's statement 'op' wrote, after 'setup': the PRESERVE
# images and the BlockMap records saying images were logged.
sub logged_by
{
	my ($setup, $op) = @_;
	sql($setup);
	my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
	sql($op);
	my $end = sql(q{SELECT pg_current_wal_insert_lsn()});
	return sql(qq{SELECT count(*) FILTER (WHERE record_type = 'PRESERVE') > 0
			AND count(*) FILTER (WHERE description LIKE '%(images logged)%') > 0
		FROM pg_get_wal_records_info('$start', '$end')
		WHERE resource_manager = 'BlockMap'});
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

# A commit keeps roots holding blocks of their own: the records say
# their images were logged.
sql(q{UPDATE app.t SET v = v + 1 WHERE id % 10 = 0});
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
sql(q{SELECT dolt_commit('-am', 'c1')});
my $end = sql(q{SELECT pg_current_wal_insert_lsn()});
ok( sql(qq{SELECT count(*) > 0 AND bool_and(description LIKE '%(images logged)%')
		FROM pg_get_wal_records_info('$start', '$end')
		WHERE resource_manager = 'BlockMap' AND description LIKE '%root captured%'}) eq 't',
	'each kept root\'s record says its own blocks\' images were logged');
ok( sql(qq{SELECT count(*) > 0 FROM pg_get_wal_records_info('$start', '$end')
		WHERE resource_manager = 'BlockMap' AND record_type = 'PRESERVE'}) eq 't',
	'and the images are there');

# A share of a relation with blocks of its own: the same.
sql(q{SET debug_block_mapped_heap = on;
	CREATE TABLE plain (a int, b text);
	INSERT INTO plain SELECT g, repeat('p', 200) FROM generate_series(1, 2000) g});
my $plain = sql(q{SELECT pg_relation_filenode('plain')});
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
is(logged_by(q{UPDATE plain SET a = a + 0 WHERE a % 10 = 1},
		qq{SELECT test_bmap_snapshot_take($plain)}),
	't', 'and a snapshot taken on its own logs its images too');

# The buffers of relation file number 'rel' written out and evicted: its
# map with them, so that recovery skips the records of pages from before
# they moved, as of a newer map, and only images rebuild what they left.
sub evict
{
	my ($rel) = @_;
	return sql(qq{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted)
		FROM pg_buffercache WHERE relfilenode = $rel});
}

# A commit's kept blocks lost by the machine, after the working set moved
# off them (its pages changed again, copied first), then a crash, with
# 'conf' appended to the configuration before the restart: recovery
# rebuilds them from the images, the checkpoint before them being where it
# begins, and a hard reset back to the commit gives them back.
my $round = 0;
sub lose_kept
{
	my ($mod, $how, $conf) = @_;
	$round++;
	sql(qq{UPDATE app.t SET v = v + 1, pad = pad || 'z' WHERE id % $mod = 0;
		SELECT dolt_commit('-am', 'lost $round')});
	my $kept = sql($contents);
	my @lost = blocks_of("t.id % $mod = 0");
	ok(@lost > 0, "the commit changed pages ($how): " . scalar(@lost));
	sql(qq{UPDATE app.t SET v = v + 1 WHERE id % $mod = 0});
	my $moved = sql($contents);
	is(scalar(grep { my $b = $_; grep { $_ == $b } @lost } blocks_of("t.id % $mod = 0")),
		0, "the working set moved off the kept blocks ($how)");
	is(evict(q{pg_relation_filenode('app.t')}), 't',
		"the table's buffers written out and evicted ($how)");
	$node->stop('immediate');
	lose_block($_) for @lost;
	$node->append_conf('postgresql.conf', $conf) if $conf;
	$node->start;
	is(sql($contents), $moved, "after a crash, the working set ($how)");
	my $restored = sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'relations_restored'});
	is(sql(q{SELECT dolt_reset('--hard'); SELECT pg_stat_force_next_flush();
		SELECT count(*), sum(v), sum(length(pad)) FROM app.t}) =~ s/^.*\n//sr,
		$kept, "after their writes were lost, a hard reset gives back the commit's pages ($how)");
	cmp_ok(sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'relations_restored'}),
		'>', $restored, "restored from the kept blocks ($how)");
}
lose_kept(7, 'at replica');

# A share's own blocks lost the same way: the copy's rows come back from
# the images, and redo of the share syncs nothing.
sql(q{UPDATE plain SET a = a + 0 WHERE a % 10 = 3});
my @shared = split /\n/, sql(qq{
	SELECT DISTINCT m.phys FROM plain p, test_bmap_map($plain) m
	WHERE p.a % 10 = 3 AND m.blkno = (p.ctid::text::point)[0]});
$digest = sql(qq{SELECT test_bmap_rows_digest($plain)});
my $copy2 = sql(qq{SELECT test_bmap_share($plain)});
sql(q{UPDATE plain SET a = a + 0 WHERE a % 10 = 3});
is(evict($plain), 't', 'the source\'s buffers written out and evicted');
my $log_offset = -s $node->logfile;
$node->append_conf('postgresql.conf', 'log_min_messages = debug1');
$node->stop('immediate');
lose_block($_) for @shared;
$node->start;
is(sql(qq{SELECT test_bmap_rows_digest($copy2)}), $digest,
	'after the share\'s blocks were lost, recovery gives the copy its rows');
ok($node->log_contains(
		'redo of a share of .* syncs nothing: its own pages were logged as images',
		$log_offset),
	'redo of the share synced nothing');

# After a checkpoint, a commit's blocks were written and synced by it, so
# a crash then needs no image: recovery begins past them.  (No write lost
# here: one the checkpoint synced isn't lost to a crash.)
sql(q{UPDATE app.t SET v = v + 1 WHERE id % 11 = 0;
	SELECT dolt_commit('-am', 'c3'); CHECKPOINT});
my $c3 = sql($contents);
$node->stop('immediate');
$node->start;
is(sql($contents), $c3, 'after a checkpoint and a crash, the commit\'s pages');

# Logged at replica, recovered at minimal: redo goes by the record.
lose_kept(13, 'logged at replica, recovered at minimal',
	"wal_level = minimal\nmax_wal_senders = 0\nmax_replication_slots = 0");

# At minimal too, a commit keeping pages logs their images, and lost writes
# come back from them.
is( logged_by(
		q{UPDATE app.t SET v = v + 1 WHERE id % 10 = 0},
		q{SELECT dolt_commit('-am', 'c5')}),
	't', 'at minimal, a commit keeping pages logs their images');
lose_kept(17, 'logged and recovered at minimal');
my $c5 = sql($contents);

# Logged at minimal, recovered at replica: redo syncs nothing.
$log_offset = -s $node->logfile;
sql(q{UPDATE app.t SET v = v + 1 WHERE id % 19 = 0;
	SELECT dolt_commit('-am', 'c6')});
$c5 = sql($contents);
$node->stop('immediate');
$node->append_conf('postgresql.conf', "wal_level = replica\nmax_wal_senders = 10");
$node->start;
is(sql($contents), $c5, 'WAL written at minimal recovers at replica');
ok($node->log_contains('redo of a snapshot of .* syncs nothing: its own pages were logged as images', $log_offset),
	'redo of a snapshot logged at minimal syncs nothing');
like(sql(q{UPDATE app.t SET v = 0; SELECT dolt_reset('--hard');
	SELECT count(*), sum(v), sum(length(pad)) FROM app.t}),
	qr/^\Q$c5\E$/m, 'and a hard reset gives back its kept pages');

$node->stop;
done_testing();
