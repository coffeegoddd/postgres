# Copyright (c) 2026, PostgreSQL Global Development Group

# The standby hold.  With block_map_copy_images off, a copy of a shared page
# may be logged without its image, and the old block then has to stay as it
# is until no recovery begins before the copy.  A standby reports where a
# recovery of it may begin (its restart point, or an online backup's start)
# and whether a backup runs on it; its physical slot keeps the report while
# it is away.  The primary keeps such blocks until every standby it knows of
# is past the copy, so the standby's replay never has to make a restartpoint
# of its own, and a backup on it never fails for them: while a backup runs
# there, copies log their images, as for one of the primary's.  A hold more
# than block_map_standby_hold_max_size of WAL behind is let go of, and the
# standby is back to making restartpoints as it replays.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('standby_hold');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
block_map_copy_images = off
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 1GB
bgwriter_lru_maxpages = 0
wal_keep_size = 1GB
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_walinspect;
	SELECT pg_create_physical_replication_slot('standby');
});
$primary->backup('base');

# A standby that makes no restartpoint of its own, reporting often.
my $standby = PostgreSQL::Test::Cluster->new('standby_hold_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
primary_slot_name = 'standby'
checkpoint_timeout = 1h
max_wal_size = 1GB
wal_receiver_status_interval = 1s
log_min_messages = debug1
});
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }

sub rows
{
	my ($node, $table) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT md5(string_agg($table\::text, ',' ORDER BY a)) FROM $table});
}

# Wait for the primary to hear a hold past, or before, a position.
sub hold_reaches
{
	my ($cond) = @_;
	return $primary->poll_query_until('postgres',
		qq{SELECT coalesce($cond, false) FROM test_bmap_standby_hold()});
}

# REMAP records from 'start' on of relation file 'filenode', and how many of
# them carry the page's image.
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

# A table whose every page is changed after a checkpoint, then shared; its
# pages are each changed again, which copies them without their images.
# Returns the copy's file number, its blocks, the pages, and where the
# copies end.
sub shared_and_copied
{
	my ($name, $images) = @_;

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
	my ($remaps, $imaged) = remaps_since($start, $t);
	is($remaps, $pages, "$name: each of the table's pages is copied");
	is($imaged, $images ? $pages : 0,
		"$name: " . ($images ? 'each copy logs its image' : 'no copy logs its image'));
	my $end = sql(q{SELECT pg_current_wal_insert_lsn()});
	return ($copy, $blocks, $pages, $end);
}

# The copy's pages made new: a change of each block in place, if claimed.
# How many of the old blocks it still names (claimed in place).
sub remake_pages
{
	my ($copy, $pages, $blocks) = @_;

	sql(qq{SELECT test_bmap_init_block($copy, b) FROM generate_series(0, $pages - 1) b});
	return sql(qq{SELECT count(*) FROM test_bmap_map($copy) WHERE phys = ANY ('{$blocks}'::int8[])});
}

my $restartpoints = qr/redo of copies without an image asks for a restartpoint/;

# The standby's restart point, at the base backup, is before the copies:
# the primary keeps the old blocks past its own checkpoint.
my ($copy, $blocks, $pages, $end) = shared_and_copied('t', 0);
my $t_rows = rows($primary, 't');
$primary->wait_for_replay_catchup($standby);
ok(hold_reaches(qq{hold < '$end'}), 'the standby reports a hold before the copies');
my $log_offset = -s $standby->logfile;
sql(q{CHECKPOINT});
sql(q{SELECT * FROM test_bmap_release_drain()});
is(remake_pages($copy, $pages, $blocks), '0',
	'past the primary\'s checkpoint, the copy copies its pages rather than claim the blocks held');
is(sql(q{SELECT freed FROM test_bmap_release_drain()}), '0', 'and a drain frees none of them');
$primary->wait_for_replay_catchup($standby);
ok(!$standby->log_contains($restartpoints, $log_offset),
	'the standby makes no restartpoint of its own');

# A crash of the standby recovers the copies from the old blocks.
$standby->stop('immediate');
$standby->start;
$primary->wait_for_replay_catchup($standby);
is(rows($standby, 't'), $t_rows, 'after a crash, the standby reads the table\'s rows');

# Its restartpoint past the copies lets the blocks go.
$standby->safe_psql('postgres', q{CHECKPOINT});
ok(hold_reaches(qq{hold >= '$end'}), 'once the standby\'s restart point passes the copies, it says so');
cmp_ok(sql(q{SELECT freed FROM test_bmap_release_drain()}), '>=', $pages,
	'and a drain frees the old blocks');
sql(qq{SELECT test_bmap_unlink($copy)});

# Away, the standby still holds, by its slot, across a restart of the
# primary too.
$standby->stop;
($copy, $blocks, $pages, $end) = shared_and_copied('u', 0);
$primary->restart;
ok(hold_reaches(qq{hold < '$end'}), 'the slot keeps the hold of a standby away, across a restart');
sql(q{CHECKPOINT});
sql(q{SELECT * FROM test_bmap_release_drain()});
is(remake_pages($copy, $pages, $blocks), '0', 'so the blocks are kept');
$log_offset = -s $standby->logfile;
$standby->start;
$primary->wait_for_replay_catchup($standby);
ok(!$standby->log_contains($restartpoints, $log_offset),
	'the standby, back, makes no restartpoint of its own');
$standby->safe_psql('postgres', q{CHECKPOINT});
ok(hold_reaches(qq{hold >= '$end'}), 'and lets them go past its restartpoint');
sql(qq{SELECT test_bmap_unlink($copy)});

# A backup on the standby: reported, copies log their images while it
# runs, and it stops as a good backup.
my $sbackup = $standby->background_psql('postgres', on_error_stop => 0);
$sbackup->query_safe(q{SET client_min_messages = warning});
$sbackup->query_safe(q{SELECT pg_backup_start('standby_hold', true)});
ok($primary->poll_query_until('postgres', q{SELECT backups FROM test_bmap_standby_hold()}),
	'a backup on the standby is reported');
sql(q{CHECKPOINT});
is(sql(q{SELECT backups FROM test_bmap_standby_hold()}), 't',
	'and stays reported as the checkpointer recomputes the hold');
($copy, $blocks, $pages, $end) = shared_and_copied('v', 1);
sql(q{CHECKPOINT});
remake_pages($copy, $pages, $blocks);
$primary->wait_for_replay_catchup($standby);
my ($out, $ret) = $sbackup->query(q{SELECT lsn FROM pg_backup_stop()});
is($ret, 0, 'the standby\'s backup stops as a good one');
$sbackup->quit;
ok($primary->poll_query_until('postgres', q{SELECT NOT backups FROM test_bmap_standby_hold()}),
	'and no backup is reported after');
sql(qq{SELECT test_bmap_unlink($copy)});

# Past the cap, the hold is let go of: the copy claims the blocks after the
# primary's checkpoint, and the standby makes a restartpoint as it replays.
$standby->safe_psql('postgres', q{CHECKPOINT});
$primary->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres', q{CHECKPOINT});
($copy, $blocks, $pages, $end) = shared_and_copied('w', 0);
my $plog = -s $primary->logfile;
$primary->append_conf('postgresql.conf', 'block_map_standby_hold_max_size = 0');
$primary->reload;
$primary->wait_for_replay_catchup($standby);
$log_offset = -s $standby->logfile;
sql(q{CHECKPOINT});
ok($primary->log_contains(qr/letting go of replication slot "standby"'s hold/, $plog),
	'past the cap, the primary lets go of the slot\'s hold, saying so');
is(remake_pages($copy, $pages, $blocks), $pages, 'and the copy claims the blocks in place');
$primary->wait_for_replay_catchup($standby);
ok($standby->log_contains($restartpoints, $log_offset),
	'the standby, no longer held for, makes a restartpoint as it replays');

# A restart publishes the slots' holds while recovery runs, when the cap
# isn't applied: so the hold is computed again as recovery ends, and a hold
# past the cap is let go of at once, not at the next checkpoint.
$primary->append_conf('postgresql.conf', 'block_map_standby_hold_max_size = -1');
$primary->reload;
# (a standby reports its hold as it changes, or as it connects)
$standby->restart;
ok($primary->poll_query_until('postgres', q{SELECT hold IS NOT NULL FROM test_bmap_standby_hold()}),
	'with no cap, the slot holds again');
$standby->stop;
$primary->append_conf('postgresql.conf', 'block_map_standby_hold_max_size = 0');
$primary->restart;
is(sql(q{SELECT hold IS NULL FROM test_bmap_standby_hold()}), 't',
	'after a restart, a hold past the cap is let go of before any checkpoint');

$primary->stop;
done_testing();
