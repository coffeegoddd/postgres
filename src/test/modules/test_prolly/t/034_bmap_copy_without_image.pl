# Copyright (c) 2026, PostgreSQL Global Development Group

# A copy of a shared page logs the page's image only as a full-page write
# would: when the page has changed since the redo pointer, its image is in
# the WAL already, and redo copies the page as replay has it in a buffer,
# or else from the old block.  So the old block keeps that page until no
# recovery begins before the copy: its other holder doesn't claim it (it
# copies the page instead), and it isn't freed (its free waits on the
# release queue), until a checkpoint passes the copy; and a standby's (or
# an archive recovery's) replay of what then changes the block first moves
# its restart point past the copy.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('copy_without_image');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
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
$primary->safe_psql('postgres', q{CREATE EXTENSION injection_points})
  if $ENV{enable_injection_points} eq 'yes';
$primary->backup('base');

# A standby that makes no restartpoint of its own: its restart point stays
# at the backup's start unless its replay asks for one.
my $standby = PostgreSQL::Test::Cluster->new('copy_without_image_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
checkpoint_timeout = 1h
max_wal_size = 1GB
log_min_messages = debug1
});
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }
sub ssql { return $standby->safe_psql('postgres', $_[0]); }

sub rows
{
	my ($node, $table) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT md5(string_agg($table\::text, ',' ORDER BY a)) FROM $table});
}

# REMAP records from 'start' on, of relation file 'filenode', and how many
# of them carry the page's image (the WAL flushed first).
sub remaps_since
{
	my ($start, $filenode) = @_;

	sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
	return split /\|/, sql(qq{
		SELECT count(*), count(*) FILTER (WHERE block_fpi_length > 0)
		FROM pg_get_wal_block_info('$start', pg_current_wal_insert_lsn(), false)
		WHERE resource_manager = 'BlockMap' AND record_type = 'REMAP'
		  AND relfilenode = $filenode AND relforknumber = 0});
}

# A table whose every page is changed after a checkpoint (so its images
# are in the WAL since the redo pointer), then shared; the table's pages
# are each changed again, which copies them away from the blocks its copy
# keeps, without their images.  Returns the table's file number, the
# copy's, and the copy's blocks.
sub shared_and_copied
{
	my ($name) = @_;

	sql(qq{
		CREATE TABLE $name (a int, b text) WITH (fillfactor = 50);
		INSERT INTO $name SELECT g, repeat('x', 200) FROM generate_series(1, 40 * 17) g;
		CHECKPOINT;
		UPDATE $name SET a = a WHERE a % 17 = 1;
	});
	my $t = sql(qq{SELECT pg_relation_filenode('$name')});
	my $pages = sql(qq{SELECT pg_relation_size('$name') / 8192});
	my $copy = sql(qq{SELECT test_bmap_share($t)});
	my $blocks = sql(qq{SELECT string_agg(phys::text, ',' ORDER BY blkno) FROM test_bmap_map($copy)});
	my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
	sql(qq{UPDATE $name SET b = 'changed'});
	my ($remaps, $images) = remaps_since($start, $t);
	is($remaps, $pages, "$name: each of the table's pages is copied");
	is($images, 0, "$name: and no copy logs the page's image");
	return ($t, $copy, $blocks, $pages);
}

# The copy's pages made new, as an access method makes a page anew: a
# change of each block in place, if claimed.
sub remake_pages
{
	my ($copy, $pages) = @_;

	sql(qq{SELECT test_bmap_init_block($copy, b) FROM generate_series(0, $pages - 1) b});
}

# Replay restores images of a page even where it skips the records after
# them (the page's map on disk newer): the copy's redo mustn't take such a
# buffer for the page.  The copy's pages are copied first, then the
# table's, each from the old block, the table's after an image of its page
# (a hint's, with checksums) and a change that replay skips.
sql(q{
	CREATE TABLE s (a int, b text) WITH (fillfactor = 50);
	INSERT INTO s SELECT g, repeat('x', 200) FROM generate_series(1, 40 * 17) g;
	CHECKPOINT;
	UPDATE s SET a = a WHERE a % 17 = 1;
});
my $s = sql(q{SELECT pg_relation_filenode('s')});
my $spages = sql(q{SELECT pg_relation_size('s') / 8192});
my $scopy = sql(qq{SELECT test_bmap_share($s)});
sql(qq{SELECT test_bmap_write($scopy, b, 7) FROM generate_series(0, $spages - 1) b});
sql(q{UPDATE s SET b = 'changed'});
my $s_rows = rows($primary, 's');
sql(q{SELECT count(pg_buffercache_evict(bufferid)) FROM pg_buffercache});
$primary->stop('immediate');
$primary->start;
is(rows($primary, 's'), $s_rows,
	'after a crash, a table copied after a skipped change reads its rows');
sql(qq{SELECT test_bmap_unlink($scopy)});
sql(q{DROP TABLE s});

# The primary: claims and frees wait for a checkpoint, and a crash before
# it recovers the copies from the old blocks, as they were.
my ($t, $copy, $blocks, $pages) = shared_and_copied('t');
my $t_rows = rows($primary, 't');

# The copy's pages, which only it holds now, are copied rather than
# claimed, and their old blocks queued, not freed.
sql(q{SELECT * FROM test_bmap_release_drain()});
remake_pages($copy, $pages);
is(sql(qq{SELECT count(*) FROM test_bmap_map($copy) WHERE phys = ANY ('{$blocks}'::int8[])}),
	'0', 'the copy copies its pages rather than claim the blocks the table\'s copies read');
is(sql(q{SELECT freed FROM test_bmap_release_drain()}), '0',
	'and a drain frees none of the blocks');

# A table made now takes none of them.
sql(q{
	CREATE TABLE w (a int, b text);
	INSERT INTO w SELECT g, repeat('w', 200) FROM generate_series(1, 2000) g;
});
my $w = sql(q{SELECT pg_relation_filenode('w')});
is(sql(qq{SELECT count(*) FROM test_bmap_map($w) WHERE phys = ANY ('{$blocks}'::int8[])}),
	'0', 'nor does a new table get one');
my $w_rows = rows($primary, 'w');

# Every page but the table's written out: its maps reach disk, naming the
# new blocks, but not its pages.  Replay from the checkpoint skips the
# table's records from before the copies (its map, newer), and has few
# buffers: each copy reads its old block.  Then a crash.
sql(qq{SELECT count(pg_buffercache_evict(bufferid)) FROM pg_buffercache
	WHERE NOT (relfilenode = $t AND relforknumber = 0)});
$primary->append_conf('postgresql.conf', 'shared_buffers = 16');
my $log_offset = -s $primary->logfile;
$primary->stop('immediate');
$primary->start;
ok($primary->log_contains(qr/redo of a copy of block \d+ of \S+ without an image reads the page from block/,
		$log_offset),
	'replay copies the pages from their old blocks');
is(rows($primary, 't'), $t_rows,
	'after a crash, the table reads its rows, copied from the old blocks');
is(rows($primary, 'w'), $w_rows, 'and the new table its rows');

# After a checkpoint the blocks are freed by a drain (a PASSED record first).
sql(q{CHECKPOINT});
my $before_free = sql(q{SELECT pg_current_wal_insert_lsn()});
cmp_ok(sql(q{SELECT freed FROM test_bmap_release_drain()}), '>=', $pages,
	'after a checkpoint, a drain frees the old blocks');
is(sql(qq{
	SELECT count(*) > 0 FROM pg_get_wal_records_info('$before_free', pg_current_wal_insert_lsn())
	WHERE resource_manager = 'BlockMap' AND record_type = 'PASSED'}),
	't', 'logging first that the copies are past the restart point');
sql(qq{SELECT test_bmap_unlink($copy)});

# A standby: the copies replayed there read the old blocks from its disk
# until its restart point passes them.  The primary, after its checkpoint,
# claims the blocks in place and changes them; the standby's replay moves
# its restart point past the copies first, and a crash then recovers them.
($t, $copy, $blocks, $pages) = shared_and_copied('u');
my $u_rows = rows($primary, 'u');
$primary->wait_for_replay_catchup($standby);
is(rows($standby, 'u'), $u_rows, 'the standby replays the copies');

$log_offset = -s $standby->logfile;
sql(q{CHECKPOINT});
remake_pages($copy, $pages);
is(sql(qq{SELECT string_agg(phys::text, ',' ORDER BY blkno) FROM test_bmap_map($copy)}),
	$blocks, 'after a checkpoint, the copy claims its blocks in place');
$primary->wait_for_replay_catchup($standby);
ok($standby->log_contains(qr/redo of copies without an image asks for a restartpoint/,
		$log_offset),
	'the standby, its restart point before the copies, makes a restartpoint');

$standby->safe_psql('postgres',
	q{SELECT count(pg_buffercache_evict(bufferid)) FROM pg_buffercache});
$standby->stop('immediate');
$standby->start;
$primary->wait_for_replay_catchup($standby);
is(rows($standby, 'u'), $u_rows,
	'after a crash, the standby\'s table reads its rows, copied from the old blocks');
sql(qq{SELECT test_bmap_unlink($copy)});

# A backup on the standby across the replay of what changes such a block
# fails as it stops: it began at a restartpoint before the copies, and
# may have copied the blocks as they changed.
($t, $copy, $blocks, $pages) = shared_and_copied('v');
$primary->wait_for_replay_catchup($standby);
my $sbackup = $standby->background_psql('postgres', on_error_stop => 0);
$sbackup->query_safe(q{SET client_min_messages = warning});
$sbackup->query_safe(q{SELECT pg_backup_start('copy_without_image', true)});
sql(q{CHECKPOINT});
remake_pages($copy, $pages);
$primary->wait_for_replay_catchup($standby);
$sbackup->query(q{SELECT * FROM pg_backup_stop()});
like($sbackup->{stderr}, qr/relies on pages the primary synced without logging them/,
	'a backup on the standby across such a replay fails');
$sbackup->quit;

# A copy while a base backup runs logs the page's image: the backup begins
# before the copy, and may copy the old block after it changes.
sql(q{
	CREATE TABLE x (a int, b text) WITH (fillfactor = 50);
	INSERT INTO x SELECT g, repeat('x', 200) FROM generate_series(1, 40 * 17) g;
	CHECKPOINT;
	UPDATE x SET a = a WHERE a % 17 = 1;
});
my $x = sql(q{SELECT pg_relation_filenode('x')});
my $xpages = sql(q{SELECT pg_relation_size('x') / 8192});
sql(qq{SELECT test_bmap_share($x)});
my $backup = $primary->background_psql('postgres');
$backup->query_safe(q{SET client_min_messages = warning});
$backup->query_safe(q{SELECT pg_backup_start('copy_without_image', true)});
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
sql(q{UPDATE x SET b = 'changed'});
my ($remaps, $images) = remaps_since($start, $x);
is($remaps, $xpages, 'during a backup, each page is copied');
is($images, $remaps, 'and each copy logs the page\'s image');
$backup->query_safe(q{SELECT * FROM pg_backup_stop()});
$backup->quit;

# A standby whose replay of what changes such a block can't make a
# restartpoint past the copies (an injection point takes it for one that
# couldn't be, as when recovery holds references to pages it couldn't
# find): it says so, and keeps the copies' position in pg_control, so that
# a crash then can't be recovered from its restart point, before them.
SKIP:
{
	skip 'Injection points not supported by this build', 5
	  if $ENV{enable_injection_points} ne 'yes';
	($t, $copy, $blocks, $pages) = shared_and_copied('y');
	$primary->wait_for_replay_catchup($standby);
	$standby->safe_psql('postgres',
		q{SELECT injection_points_attach('bmap-redo-no-restartpoint', 'notice')});
	$log_offset = -s $standby->logfile;
	sql(q{CHECKPOINT});
	remake_pages($copy, $pages);
	$primary->wait_for_replay_catchup($standby);
	ok($standby->log_contains(qr/restart point of recovery .* is not past copies made without an image/,
			$log_offset),
		'a standby that makes no restartpoint past the copies says so');
	$standby->stop('immediate');
	$log_offset = -s $standby->logfile;
	ok(!$standby->start(fail_ok => 1),
		'and refuses to recover from before them after a crash');
	ok($standby->log_contains(qr/recovery would begin at .*, not past pages synced without being logged/,
			$log_offset),
		'saying why');
}

$primary->stop;
done_testing();
