# Copyright (c) 2026, PostgreSQL Global Development Group

# A share makes the source's own blocks durable by writing them out and
# syncing the page store, not by logging their images, but for blocks the
# source got since the last checkpoint.  Recovery may skip the source's
# records from before the share (its map on disk newer: a path copied
# since) after writing something else over a block (zeroing it again as
# it replays the block's allocation), so it must never begin before the
# source got a block it didn't log: crash recovery begins at the
# checkpoint, and a standby's replay of the share makes a restartpoint
# past it.  A backup that may copy a block before the share writes it gets
# the images after all.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::RecursiveCopy;

my $primary = PostgreSQL::Test::Cluster->new('share_synced_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
block_map_standby_hold_max_size = 0  # no standby hold: the fallbacks, as tested here
checkpoint_timeout = 1h
max_wal_size = 1GB
bgwriter_lru_maxpages = 0
wal_keep_size = 1GB
log_min_messages = debug1
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_buffercache;
	CREATE EXTENSION pg_walinspect;
});
$primary->backup('base');

# A standby that makes no restartpoint of its own: its restart point stays
# at the backup's start.
my $standby = PostgreSQL::Test::Cluster->new('share_synced_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
checkpoint_timeout = 1h
max_wal_size = 1GB
log_min_messages = debug1
});
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }
sub ssql { return $standby->safe_psql('postgres', $_[0]); }

# Full-page images of relation file 'filenode''s main fork from 'start' on
# (the WAL flushed first: a share outside a transaction doesn't).
sub images_since
{
	my ($start, $filenode) = @_;

	sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
	return sql(qq{
		SELECT count(*) FROM pg_get_wal_block_info('$start', pg_current_wal_insert_lsn(), false)
		WHERE relfilenode = $filenode AND relforknumber = 0 AND block_fpi_length > 0});
}

# A table of about 'pages' pages, rows frozen.
sub make_table
{
	my ($name, $pages) = @_;

	sql(qq{
		CREATE TABLE $name (a int, b text) WITH (fillfactor = 50);
		INSERT INTO $name SELECT g, repeat('x', 200)
			FROM generate_series(1, $pages * 17) g;
		VACUUM (FREEZE) $name;
	});
	return sql(qq{SELECT pg_relation_filenode('$name')});
}

# Crash recovery: a table with blocks it got before the checkpoint, and
# blocks freed by a dropped table that it took after it (each zeroed again
# by its allocation's replay).  The share logs images of those alone.
my $old = make_table('old', 40);
make_table('gone', 10);
sql(q{CHECKPOINT});
sql(q{DROP TABLE gone});
sql(q{SELECT * FROM test_bmap_release_drain()});
sql(q{INSERT INTO old SELECT g, repeat('y', 200) FROM generate_series(1, 10 * 17) g});
my $pages = sql(q{SELECT pg_relation_size('old') / 8192});
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
my $digest = sql(qq{SELECT test_bmap_rows_digest($old)});
my $copy = sql(qq{SELECT test_bmap_share($old)});
my $images = images_since($start, $old);
cmp_ok($images, '>', 0, 'the share logs images of the blocks taken since the checkpoint');
cmp_ok($images, '<', $pages / 2, 'and of no block taken before it');

# The source changes its first page after the share, which copies the path
# to it, from its subtree's top: its map, written out, is newer than the
# records recovery replays from the checkpoint for every page under the
# top, which it skips (and the pages it took after the checkpoint it
# doesn't copy, so no copy's image of them is logged).  The copy reads the
# pages as of the share all the same.
sql(q{UPDATE old SET b = 'z' WHERE a = 1});
sql(q{SELECT pg_buffercache_evict_relation('old')});
$primary->stop('immediate');
$primary->start;
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'after a crash, the copy reads the pages as of the share');

# A standby: a table made before the standby's restart point (a recovery
# from before it would make its map again), with blocks it took after it
# (freed by a dropped table, zeroed again by their allocations' replay)
# and before the primary's checkpoint, so the share logs no image of them.
# Replaying the share, the standby makes a restartpoint past them.
my $taken = make_table('taken', 10);
make_table('freed', 20);
sql(q{CHECKPOINT});
$primary->wait_for_replay_catchup($standby);
ssql(q{CHECKPOINT});
sql(q{DROP TABLE freed});
sql(q{SELECT * FROM test_bmap_release_drain()});
sql(q{INSERT INTO taken SELECT g, repeat('y', 200) FROM generate_series(1, 20 * 17) g});
sql(q{CHECKPOINT});
$start = sql(q{SELECT pg_current_wal_insert_lsn()});
$digest = sql(qq{SELECT test_bmap_rows_digest($taken)});
my $log_offset = -s $standby->logfile;
$copy = sql(qq{SELECT test_bmap_share($taken)});
is(images_since($start, $taken), 0, 'a share of blocks taken before the checkpoint logs no image');
$primary->wait_for_replay_catchup($standby);
ok($standby->log_contains(qr/redo of a share of .* asks for a restartpoint/, $log_offset),
	'the standby, its restart point before the blocks, makes a restartpoint');
ok($standby->log_contains(qr/redo of a share of .* synced its own pages/, $log_offset),
	'having synced the shared blocks');

# The source's map, newer (its first page copied), reaches the standby's
# disk, which crashes: its replay skips the source's records from before
# the share.
sql(q{UPDATE taken SET b = 'z' WHERE a = 1});
$primary->wait_for_replay_catchup($standby);
ssql(q{SELECT pg_buffercache_evict_relation('taken')});
$standby->stop('immediate');
$standby->start;
$primary->wait_for_replay_catchup($standby);
is(ssql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'after a crash, the standby\'s copy reads the pages as of the share');

# A share while a base backup runs logs an image of every own block: the
# backup may copy a block before the share writes it.
my $backing = make_table('backing', 20);
sql(q{CHECKPOINT});
my $backup = $primary->background_psql('postgres');
$backup->query_safe(q{SET client_min_messages = warning});
$backup->query_safe(q{SELECT pg_backup_start('share_synced', true)});
$start = sql(q{SELECT pg_current_wal_insert_lsn()});
sql(qq{SELECT test_bmap_share($backing)});
cmp_ok(images_since($start, $backing), '>=', sql(q{SELECT pg_relation_size('backing') / 8192}),
	'a share during a backup logs an image of each own block');
$backup->query_safe(q{SELECT * FROM pg_backup_stop()});
$backup->quit;

# A base backup on the standby while it replays a share that logged no
# image fails as it stops: it may have copied a block before the replay
# wrote it.
my $late = make_table('late', 20);
sql(q{CHECKPOINT});
$primary->wait_for_replay_catchup($standby);
my $sbackup = $standby->background_psql('postgres', on_error_stop => 0);
$sbackup->query_safe(q{SET client_min_messages = warning});
$sbackup->query_safe(q{SELECT pg_backup_start('share_synced_standby', true)});
my $late_copy = sql(qq{SELECT test_bmap_share($late)});
my $late_digest = sql(qq{SELECT test_bmap_rows_digest($late)});
$primary->wait_for_replay_catchup($standby);
$sbackup->query(q{SELECT * FROM pg_backup_stop()});
like($sbackup->{stderr}, qr/relies on pages the primary synced without logging them/,
	'a backup on the standby during the replay of a synced share fails');
$sbackup->quit;

# One begun after the replay stops as usual, and a server restored from
# it reads the copy, the source changed since.
$sbackup = $standby->background_psql('postgres');
$sbackup->query_safe(q{SET client_min_messages = warning});
$sbackup->query_safe(q{SELECT pg_backup_start('share_synced_standby_after', true)});
$sbackup->query_safe(q{SELECT * FROM pg_backup_stop()});
pass('a backup on the standby begun after the replay stops');
$sbackup->quit;
sql(q{UPDATE late SET b = 'z' WHERE a % 5 = 0});
$primary->wait_for_replay_catchup($standby);
$standby->backup('after_replay');
my $restored = PostgreSQL::Test::Cluster->new('share_synced_restored');
$restored->init_from_backup($standby, 'after_replay', has_streaming => 1);
$restored->start;
$standby->wait_for_replay_catchup($restored, $primary);
is($restored->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($late_copy)}),
	$late_digest, 'a server restored from a standby backup after the replay reads the copy');
$restored->stop;

# Injection points: a backup begun while a share syncs its blocks.
SKIP:
{
	skip 'Injection points not supported by this build', 9
	  if $ENV{enable_injection_points} ne 'yes';

	sql(q{CREATE EXTENSION injection_points});
	my $during = make_table('during', 20);
	sql(q{CHECKPOINT});
	my $during_digest = sql(qq{SELECT test_bmap_rows_digest($during)});
	$start = sql(q{SELECT pg_current_wal_insert_lsn()});
	$log_offset = -s $primary->logfile;
	sql(q{SELECT injection_points_attach('bmap-share-sync', 'wait')});
	my $sharing = $primary->background_psql('postgres');
	$sharing->query_until(qr/started/, qq{
		\\echo started
		SELECT test_bmap_share($during) AS during_copy \\gset
	});
	$primary->wait_for_event('client backend', 'bmap-share-sync');
	my $during_backup = $primary->background_psql('postgres');
	$during_backup->query_safe(q{SET client_min_messages = warning});
	$during_backup->query_safe(q{SELECT pg_backup_start('share_synced_during', true)});
	sql(q{SELECT injection_points_detach('bmap-share-sync')});
	sql(q{SELECT injection_points_wakeup('bmap-share-sync')});
	my $during_copy = $sharing->query_safe(q{\\echo :during_copy});
	$sharing->quit;
	ok($primary->log_contains(qr/a share of .* logged images of \d+ blocks it synced, a backup having begun/,
			$log_offset),
		'a backup begun while a share syncs its blocks gets their images');
	cmp_ok(images_since($start, $during), '>=', sql(q{SELECT pg_relation_size('during') / 8192}),
		'logged after the share\'s record');

	# The backup's files copied, the source's map newer than the share (its
	# first page copied, and written out), and the backup restored: it
	# reads the copy and the source as they were.
	sql(q{UPDATE during SET b = 'z' WHERE a = 1});
	my $during_rows = sql(qq{SELECT test_bmap_rows_digest($during)});
	sql(q{SELECT pg_buffercache_evict_relation('during')});
	my $backup_dir = $primary->backup_dir . '/during_share';
	PostgreSQL::Test::RecursiveCopy::copypath($primary->data_dir, $backup_dir,
		filterfn => sub {
			my $src = shift;
			return $src ne 'postmaster.pid' && $src ne 'postmaster.opts' &&
			  $src !~ m{^pg_wal/.};
		});
	my $label = $during_backup->query_safe(q{SELECT labelfile FROM pg_backup_stop()});
	$during_backup->quit;
	PostgreSQL::Test::Utils::append_to_file("$backup_dir/backup_label", $label);
	my $from_during = PostgreSQL::Test::Cluster->new('share_synced_during');
	$from_during->init_from_backup($primary, 'during_share', has_streaming => 1);
	$from_during->start;
	$primary->wait_for_replay_catchup($from_during);
	is($from_during->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($during_copy)}),
		$during_digest, 'a server restored from a backup begun during the share reads the copy');
	is($from_during->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($during)}),
		$during_rows, 'and the source');
	$from_during->stop;

	# A share's replay whose restartpoint can't be made (an injection point
	# takes it for one that couldn't be, as when recovery holds references
	# to pages it couldn't find): the standby says so, and a crash then
	# can't be recovered from its restart point, before the blocks.
	my $unsafe = make_table('unsafe', 10);
	sql(q{CHECKPOINT});
	$primary->wait_for_replay_catchup($standby);
	$standby->safe_psql('postgres',
		q{SELECT injection_points_attach('bmap-redo-no-restartpoint', 'notice')});
	$log_offset = -s $standby->logfile;
	sql(qq{SELECT test_bmap_share($unsafe)});
	$primary->wait_for_replay_catchup($standby);
	ok($standby->log_contains(qr/restart point of recovery .* is not past the blocks a share of .* synced/,
			$log_offset),
		'a standby that makes no restartpoint past the blocks says so');
	$standby->stop('immediate');
	$log_offset = -s $standby->logfile;
	ok(!$standby->start(fail_ok => 1), 'and refuses to recover from before them after a crash');
	ok($standby->log_contains(qr/recovery would begin at .*, not past pages synced without being logged/,
			$log_offset),
		'saying why');

	# A standby whose restart point isn't past such pages refuses an online
	# backup, which would begin there, until a restartpoint is.
	$primary->backup('again');
	my $standby2 = PostgreSQL::Test::Cluster->new('share_synced_standby2');
	$standby2->init_from_backup($primary, 'again', has_streaming => 1);
	$standby2->append_conf('postgresql.conf', qq{
checkpoint_timeout = 1h
max_wal_size = 1GB
});
	$standby2->start;
	$primary->wait_for_replay_catchup($standby2);
	$standby2->safe_psql('postgres',
		q{SELECT test_synced_pages_note(pg_last_wal_replay_lsn())});
	my ($ret, $out, $err) = $standby2->psql('postgres',
		q{SELECT pg_backup_start('refused', true)});
	like($err, qr/is not past pages replay synced without logging them/,
		'a standby refuses a backup beginning before pages synced');
	sql(q{CHECKPOINT});
	$primary->wait_for_replay_catchup($standby2);
	($ret, $out, $err) = $standby2->psql('postgres',
		q{SELECT pg_backup_start('allowed', true); SELECT pg_backup_stop()});
	is($ret, 0, "and allows one once a restartpoint can be made past them ($err)");
	$standby2->stop;
}

$standby->stop if $ENV{enable_injection_points} ne 'yes';
$primary->stop;
done_testing();
