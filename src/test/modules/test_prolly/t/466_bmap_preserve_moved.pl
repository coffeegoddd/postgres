# Copyright (c) 2026, PostgreSQL Global Development Group

# A share walking its relation's pages again (it let go of the map's header
# to wait for a page a row lock held) while a compaction moved them: the
# pages moved are logged at their new blocks.  A base backup taken across
# it, its files copied while the share had let go of the header, restores
# the copy's rows from the images both walks logged.

use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::RecursiveCopy;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('preserve_moved');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.scratch (a int);
	INSERT INTO cow.scratch VALUES (1);
	CREATE TABLE cow.f (a int, b text);
	INSERT INTO cow.f SELECT g, repeat('f', 200) FROM generate_series(1, 1000) g;
	CREATE TABLE cow.k (a int PRIMARY KEY, b text);
	INSERT INTO cow.k SELECT g, repeat('k', 200) FROM generate_series(1, 100) g;
});
my $scratch = sql(q{SELECT pg_relation_filenode('cow.scratch')});
my $k = sql(q{SELECT pg_relation_filenode('cow.k')});
is(sql(qq{SELECT count(*) FROM test_bmap_map($k)}), '3', 'three pages');
# f's blocks, below k's, freed for a compaction to move k's into
sql(q{DROP TABLE cow.f});
sql(q{SELECT * FROM test_bmap_release_drain()});
my $kmap = sql(qq{SELECT string_agg(phys::text, ',' ORDER BY blkno) FROM test_bmap_map($k)});

# A base backup begins.
my $backup = $node->background_psql('postgres');
$backup->query_safe(q{SELECT pg_backup_start('moved', true)});

# A compaction stopped before its first move.
my $sweep = $node->background_psql('postgres');
$sweep->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-compact-room', 'wait')});
$sweep->query_until(qr/started/, q{
	\echo started
	SELECT 'moved ' || moved FROM prolly_page_sweep();
	\echo done
});
$node->wait_for_event('client backend', 'bmap-compact-room');

# A row lock of k's last page, planned before (a plan reads the header).
my $reader = $node->background_psql('postgres', on_error_stop => 0);
$reader->query_safe(q{SET plan_cache_mode = force_generic_plan;
	PREPARE l AS SELECT a FROM cow.k WHERE a = 100 FOR UPDATE;
	BEGIN; EXECUTE l; COMMIT});
sql(qq{SELECT test_bmap_share($scratch)});
sql(q{SELECT pg_stat_reset_shared('prolly')});

# The share stops at its first page, holding the header; the lock waits
# for it, holding the last page.
my $op = $node->background_psql('postgres', on_error_stop => 0);
$op->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-preserve-page', 'wait');
	SELECT injection_points_attach('bmap-preserve-restart', 'wait')});
$op->query_until(qr/started/,
	qq{\\echo started\nSELECT test_bmap_share($k) AS made \\gset\nSELECT pg_stat_force_next_flush();\n});
$node->wait_for_event('client backend', 'bmap-preserve-page');
$reader->query_until(qr/started/, qq{\\echo started\nBEGIN; EXECUTE l; COMMIT;\n});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event_type = 'Buffer' AND wait_event = 'BufferShared'})
  or die "the reader didn't wait for the header";

# The share logs the first two pages, gives up on the last, and lets go of
# the header.
sql(q{SELECT injection_points_detach('bmap-preserve-page')});
sql(q{SELECT injection_points_wakeup('bmap-preserve-page')});
$node->wait_for_event('client backend', 'bmap-preserve-restart');

# Meanwhile the compaction moves k's pages, and the backup copies the files.
sql(q{SELECT injection_points_detach('bmap-compact-room')});
sql(q{SELECT injection_points_wakeup('bmap-compact-room')});
my $swept = $sweep->query_until(qr/done/, '');
like($swept, qr/moved [1-9]/, 'the compaction moved blocks');
isnt(sql(qq{SELECT string_agg(phys::text, ',' ORDER BY blkno) FROM test_bmap_map($k)}),
	$kmap, 'k\'s among them');
my $copied = $node->backup_dir . '/moved';
PostgreSQL::Test::RecursiveCopy::copypath(
	$node->data_dir, $copied,
	filterfn => sub {
		my $src = shift;
		return $src ne 'postmaster.pid' && $src ne 'postmaster.opts'
		  && $src !~ /^pg_wal\// && $src ne 'pg_wal';
	});

# The share walks again: the pages moved are logged at their new blocks.
sql(q{SELECT injection_points_detach('bmap-preserve-restart')});
sql(q{SELECT injection_points_wakeup('bmap-preserve-restart')});
my $made = $op->query_safe(q{SELECT :'made'});
$reader->quit;
ok($node->poll_query_until('postgres', q{SELECT value = 5 FROM pg_stat_prolly
	WHERE name = 'bmap_pages_preserved'}),
	'the two pages logged before the move logged again, and the last');
my $rows = sql(qq{SELECT test_bmap_rows_digest($k)});
is(sql(qq{SELECT test_bmap_rows_digest($made)}), $rows, 'the copy has the rows');

# The backup ends; restored, it has the copy's rows.
my ($label) = $backup->query_safe(q{SELECT labelfile FROM pg_backup_stop(false)});
$backup->quit;
$op->quit;
$sweep->quit;
mkdir "$copied/pg_wal";
opendir(my $dh, $node->data_dir . '/pg_wal') or die;
for my $f (readdir $dh)
{
	next unless $f =~ /^[0-9A-F]{24}$/;
	copy($node->data_dir . "/pg_wal/$f", "$copied/pg_wal/$f") or die "copy $f: $!";
}
closedir $dh;
mkdir "$copied/pg_wal/archive_status";
open(my $lf, '>', "$copied/backup_label") or die;
print $lf $label;
close $lf;
my $restored = PostgreSQL::Test::Cluster->new('preserve_moved_restored');
$restored->init_from_backup($node, 'moved');
$restored->start;
is($restored->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($made)}), $rows,
	'a base backup taken across the walks restores the copy\'s rows');
is($restored->safe_psql('postgres', q{SELECT count(*) FROM cow.k}), '100', 'and the source\'s');
$restored->stop;

# And a crash recovery.
$node->stop('immediate');
$node->start;
is(sql(qq{SELECT test_bmap_rows_digest($made)}), $rows, 'so does a crash recovery');
$node->stop;
done_testing();
