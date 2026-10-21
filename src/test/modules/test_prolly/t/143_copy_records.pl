# Copyright (c) 2026, PostgreSQL Global Development Group

# A collection WAL-logs the chunks it copies from the store's own files as
# where they come from, not as their bytes (#564): its WAL is a small part
# of what it copies (prolly.log_copied_bytes logs the bytes, to compare).
# Redo copies again from its own files: a standby's files are the
# primary's, byte for byte; crash recovery replays copies whose sources a
# retire removed since, and copies cut short before their swap; a backup
# whose redo replays a collection holds the files the collection copied
# from (none is removed while a backup runs, on a primary or a standby);
# recovery to a point between a copy and its source's removal reads the
# store; and a standby whose files differ from the primary's stops, rather
# than writing what the WAL didn't say.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::RecursiveCopy;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('copies');
$node->init(allows_streaming => 1, has_archiving => 1);
# (WAL kept for the standby across the primary's crashes and checkpoints)
$node->append_conf('postgresql.conf', "autovacuum = off\nwal_keep_size = '256MB'");
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE cp');
$node->safe_psql('cp', 'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
$node->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('copies_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('cp', $_[0]); }
my $oid = sql(q{SELECT oid FROM pg_database WHERE datname = 'cp'});

# the store's files, each with its md5
my $files = q{SELECT string_agg(f || ' ' || md5(pg_read_binary_file(d || '/' || f)), E'\n' ORDER BY f)
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'cp') db,
		pg_ls_dir(d) f
	WHERE (f LIKE 'prolly\_journal%' OR f LIKE 'prolly\_table\_%'
		OR f = 'prolly_manifest')};
my $table_bytes = q{SELECT coalesce(sum(bytes), 0) FROM test_chunk_table_files()};
my $named = q{SELECT string_agg(name, ' ' ORDER BY name) FROM test_chunk_table_files()};
my $segments = q{SELECT string_agg(name, ' ' ORDER BY name) FROM test_chunk_segments()};
# the table files on disk
my $on_disk = q{SELECT string_agg(f, ' ' ORDER BY f)
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'cp') db,
		pg_ls_dir(d) f
	WHERE f LIKE 'prolly\_table\_%'};

# Chunks of 32 kB of noise (LZ4 can't shrink them), by a name and a
# number; the live ones listed (in a heap table, so standbys and backups
# have the list), under a root naming them all.
sql(q{CREATE TABLE live (hash text PRIMARY KEY, md5 text) USING heap});

sub put
{
	my ($name, $count, $live) = @_;
	my $chunk = "test_noise('$name ' || g, 32768)";

	if (!$live)
	{
		sql(qq{SELECT test_chunk_put($chunk) FROM generate_series(1, $count) g});
		return;
	}
	sql(qq{INSERT INTO live SELECT test_chunk_put($chunk), md5($chunk)
		FROM generate_series(1, $count) g});
	my $refs = sql(q{SELECT array_agg(hash ORDER BY hash) FROM live});
	my $root = sql("SELECT test_chunk_put_node(1, '$refs')");
	sql("SELECT test_chunk_move_root(test_chunk_root(), '$root')");
}

# The first count live chunks (by hash) let go, the root moved past them.
sub drop_live
{
	my ($count) = @_;
	sql(qq{DELETE FROM live WHERE hash IN
		(SELECT hash FROM live ORDER BY hash LIMIT $count)});
	put('none', 0, 1);
}

my $reads = q{SELECT count(*) FILTER (WHERE md5(test_chunk_get(hash)) = md5)
	|| ' of ' || count(*) FROM live};
sub all_read { my $c = sql('SELECT count(*) FROM live'); return "$c of $c"; }

# A collection's WAL, and what it copied: (WAL bytes, bytes copied).
my $gc_copied = q{SELECT value FROM pg_stat_prolly WHERE name = 'gc_bytes_copied'};
sub collect
{
	my ($full, $settings) = @_;
	my $before = $node->lsn('insert');
	my $copied_before = sql($gc_copied);
	my $out = $node->safe_psql('cp', qq{$settings
		SELECT test_chunk_gc($full);
		SELECT pg_stat_force_next_flush();
		SELECT ($gc_copied) - $copied_before});
	my $copied = (split(/\n/, $out))[-1];
	my $wal = $node->safe_psql('cp',
		"SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '$before')");
	return ($wal, $copied);
}

# A collection of the journal: its chunk records copied.
put('live', 200, 1);
put('dead', 200, 0);
my ($wal, $copied) = collect('false', '');
cmp_ok($copied, '>', 200 * 32768, 'the collection copied the live chunks to a table file');
cmp_ok($wal, '<', $copied / 50,
	"its WAL is a small part of what it copied ($wal of $copied bytes)");
note "collection of the journal: $wal bytes of WAL for $copied bytes copied";
is(sql($reads), all_read(), 'the live chunks read from the file');

# A full collection of the table file, more than half of it garbage (less
# is kept whole): runs of its bytes copied.
drop_live(120);
($wal, $copied) = collect('true', '');
cmp_ok($copied, '>', 80 * 32768, 'a full collection copied the table file\'s live chunks');
cmp_ok($wal, '<', $copied / 50,
	"its WAL is a small part of what it copied ($wal of $copied bytes)");
note "full collection of a table file: $wal bytes of WAL for $copied bytes copied";

# The same with prolly.log_copied_bytes: the bytes logged.
drop_live(50);
($wal, $copied) = collect('true', 'SET prolly.log_copied_bytes = on;');
cmp_ok($wal, '>', $copied,
	"with prolly.log_copied_bytes, the bytes are logged ($wal of $copied bytes)");
note "the same, its bytes logged: $wal bytes of WAL for $copied bytes copied";
is(sql($reads), all_read(), 'and the chunks read');

# A standby replays the copies from its own files.
$node->wait_for_catchup($standby);
is($standby->safe_psql('cp', $files), sql($files),
	"the standby's store files are the primary's, byte for byte");
is($standby->safe_psql('cp', $reads), all_read(), 'and it reads the chunks');

# Crash recovery after a collection, from a checkpoint before it: its
# copies' sources were retired since, and the files they wrote, synced
# before the swap, hold their bytes.
put('crash live', 20, 1);
put('crash dead', 20, 0);
drop_live(10);
sql('CHECKPOINT');
collect('true', '');
my $after = sql($files);
$node->stop('immediate');
$node->start;
is(sql($reads), all_read(), 'crash recovery replays copies whose sources are gone');
is(sql($files), $after, 'and leaves the files as they were');

# Two collections after the checkpoint, the second retiring what the first
# wrote: the first's copies replay with their sources and their file both
# gone, so they wait, and the second's retire of that file resolves them.
put('twice live', 20, 1);
put('twice dead', 20, 0);
drop_live(10);
sql('CHECKPOINT');
collect('true', '');
drop_live(5);
collect('true', '');
$after = sql($files);
$node->stop('immediate');
$node->start;
is(sql($reads), all_read(),
	'crash recovery replays copies whose file a later retire removed');
is(sql($files), $after, 'and leaves the files as they were');

# A collection that fails after its copies, then one that collects what
# the first copied from: the first's copies wait the same way, and the
# second's removal of the unnamed file the first wrote resolves them.
put('failed live', 20, 1);
put('failed dead', 20, 0);
drop_live(10);
sql('CHECKPOINT');
my ($failed_ok, $failed_out, $failed_err) = $node->psql('cp', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-copied', 'error');
	SELECT test_chunk_gc(true);});
like($failed_err, qr/prolly-gc-copied/, 'a collection fails after its copies');
collect('true', '');
$after = sql($files);
$node->stop('immediate');
$node->start;
is(sql($reads), all_read(),
	'crash recovery replays copies whose file the next writer removed');
is(sql($files), $after, 'and leaves the files as they were');

# A crash after a collection's copies, before its swap: the copies replay
# from their sources, and the new file, never named, is removed.
put('cut live', 20, 1);
drop_live(10);
my $g = $node->background_psql('cp');
$g->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-copied', 'wait')});
$g->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_gc(true);
});
$node->poll_query_until('cp', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-gc-copied'}) or die 'the collection did not stop';
$node->stop('immediate');
$g->{run}->kill_kill;
$node->start;
is(sql($reads), all_read(), 'a crash between the copies and the swap leaves the store whole');
is(sql($on_disk), sql($named), 'the file the copies wrote, unnamed, is removed');

# Recovery to a point between a collection's copies and its retire, from a
# backup before them: the sources are there, the file the copies wrote is
# unnamed, and the store reads as it was.
$node->backup('before_copies');
put('pitr live', 20, 1);
put('pitr dead', 20, 0);
drop_live(10);
my $pitr_root = sql('SELECT test_chunk_root()');
my $pitr_reads = all_read();
$g = $node->background_psql('cp');
$g->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-copied', 'wait')});
$g->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_gc(true);
	\echo done
});
$node->poll_query_until('cp', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-gc-copied'}) or die 'the collection did not stop';
my $target = $node->lsn('insert');
sql(q{SELECT injection_points_wakeup('prolly-gc-copied')});
$g->query_until(qr/done/, '');
$g->quit;
sql('SELECT pg_switch_wal()');
my $pitr = PostgreSQL::Test::Cluster->new('copies_pitr');
$pitr->init_from_backup($node, 'before_copies', has_restoring => 1,
	standby => 0);
$pitr->append_conf('postgresql.conf', qq{
recovery_target_lsn = '$target'
recovery_target_action = 'promote'
archive_mode = off
});
$node->poll_query_until('postgres', qq{SELECT last_archived_wal >= pg_walfile_name('$target')
	FROM pg_stat_archiver}) or die 'the WAL was not archived';
$pitr->start;
$pitr->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die 'the recovery did not end';
is($pitr->safe_psql('cp', $reads), $pitr_reads,
	'recovery to between the copies and the retire reads the store');
is($pitr->safe_psql('cp', 'SELECT test_chunk_root()'), $pitr_root, 'at its root');
$pitr->stop;

# A backup taken while a collection runs: the backup lists the store's
# files before the collection and reads them after, as pg_basebackup may.
# The files the collection retires stay while a backup runs, so the
# backup has them, and its redo copies from them.
put('backup live', 20, 1);
put('backup dead', 20, 0);
drop_live(10);
my $backup_root = sql('SELECT test_chunk_root()');
my %listed = do
{
	opendir(my $d, $node->data_dir . "/base/$oid") or die;
	map { $_ => 1 } grep { /^prolly_(journal|table_)/ } readdir($d);
};
my $b = $node->background_psql('postgres');
$b->query_safe(q{SET client_min_messages = warning;
	SELECT pg_backup_start('copies', true)});
my @retiring = split(/ /, sql($segments));
collect('true', '');
my @kept = grep { -e $node->data_dir . "/base/$oid/$_" } @retiring;
is(scalar(@kept), scalar(@retiring),
	'the files a collection retires stay while a backup runs');
my $backup_dir = $node->backup_dir . '/during_copies';
PostgreSQL::Test::RecursiveCopy::copypath($node->data_dir, $backup_dir,
	filterfn => sub {
		my $src = shift;
		return 0 if $src eq 'postmaster.pid' || $src =~ m!^pg_wal/.!;
		if ($src =~ m!^base/$oid/(prolly_(journal|table_)[^/]*)$!)
		{
			return $listed{$1} ? 1 : 0;
		}
		return 1;
	});
my $label = $b->query_safe(q{SELECT labelfile FROM pg_backup_stop(true)});
$b->quit;
PostgreSQL::Test::Utils::append_to_file("$backup_dir/backup_label", "$label\n");
sql('SELECT pg_switch_wal()');
my $restored = PostgreSQL::Test::Cluster->new('copies_restored');
$restored->init_from_backup($node, 'during_copies', has_restoring => 1,
	standby => 0);
$restored->append_conf('postgresql.conf', 'archive_mode = off');
$restored->start;
$restored->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die 'the restore did not finish';
is($restored->safe_psql('cp', $reads), all_read(), 'the backup restored reads the store');
is($restored->safe_psql('cp', 'SELECT test_chunk_root()'), $backup_root, 'at its root');
$restored->stop;

# Once the backup ends, the next writer removes them.
collect('false', '');
@kept = grep { -e $node->data_dir . "/base/$oid/$_" } @retiring;
is(scalar(@kept), 0, 'and the next writer removes them once it ends');

# A standby's own backup keeps them too; its redo removes them once it
# ends.
$node->wait_for_catchup($standby);
put('standby live', 20, 1);
put('standby dead', 20, 0);
drop_live(10);
@retiring = split(/ /, sql($segments));
$b = $standby->background_psql('postgres');
$b->query_safe(q{SET client_min_messages = warning;
	SELECT pg_backup_start('standby copies', true)});
collect('true', '');
$node->wait_for_catchup($standby);
@kept = grep { -e $standby->data_dir . "/base/$oid/$_" } @retiring;
is(scalar(@kept), scalar(@retiring),
	'a standby keeps the files a retire removes while its backup runs');
$b->query_safe(q{SELECT pg_backup_stop(false)});
$b->quit;
sql(q{SELECT test_chunk_put('after the standby backup')});
$node->wait_for_catchup($standby);
@kept = grep { -e $standby->data_dir . "/base/$oid/$_" } @retiring;
is(scalar(@kept), 0, 'and removes them once it ends');
is($standby->safe_psql('cp', $files), sql($files),
	"the standby's store files are still the primary's");

# A standby restarted while it keeps them for its backup, from a
# restartpoint past the retire: redo doesn't replay the retire again, and
# no writer runs there; it removes them as it reaches a consistent state.
put('restart live', 20, 1);
put('restart dead', 20, 0);
drop_live(10);
@retiring = split(/ /, sql($segments));
$b = $standby->background_psql('postgres');
$b->query_safe(q{SET client_min_messages = warning;
	SELECT pg_backup_start('standby restart', true)});
collect('true', '');
my $retired_at = $node->lsn('insert');
sql('CHECKPOINT');
$node->wait_for_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');
@kept = grep { -e $standby->data_dir . "/base/$oid/$_" } @retiring;
is(scalar(@kept), scalar(@retiring), 'the standby keeps them while its backup runs');
is( $standby->safe_psql('postgres',
		qq{SELECT redo_lsn >= '$retired_at' FROM pg_control_checkpoint()}),
	't', 'its restartpoint is past the retire');
$b->{run}->kill_kill;
$standby->stop;
$standby->start;
@kept = grep { -e $standby->data_dir . "/base/$oid/$_" } @retiring;
is(scalar(@kept), 0, 'restarted from past the retire, it removes them');
is($standby->safe_psql('cp', $files), sql($files),
	"and its store files are the primary's");

# A standby whose source differs from the primary's stops at the copy,
# rather than writing bytes the WAL never had: a live chunk's record in
# the standby's journal changed, then collected.
put('differs live', 5, 1);
$node->wait_for_catchup($standby);
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');
$standby->poll_query_until('postgres',
	q{SELECT pg_get_wal_replay_pause_state() = 'paused'}) or die 'replay did not pause';
my $file = (split(/ /, sql($segments)))[-1];
my $path = $standby->data_dir . "/base/$oid/$file";
open(my $fh, '+<', $path) or die "could not open $path: $!";
binmode $fh;
seek($fh, 200, 0);
print $fh 'X' x 16;
close($fh);
collect('true', '');
my $log_offset = -s $standby->logfile;
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');
ok( $standby->wait_for_log(
		qr/PANIC: .*could not replay a copy into chunk store file/, $log_offset),
	'a standby whose source differs PANICs at the copy');
like(slurp_file($standby->logfile, $log_offset),
	qr/has no chunk record of \d+ bytes at \d+/,
	'saying which source failed it');
$standby->stop('immediate', fail_ok => 1);

done_testing();
