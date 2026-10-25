# Copyright (c) 2026, PostgreSQL Global Development Group

# A page a share's first walk logged, changed in place (a row lock, a
# prune) while the share has let go of the map's header to wait for another
# page: the walk again logs it again (redo of the share takes the images for
# the pages as of the share), and skips the page logged and unchanged.  The
# source and the copy have the changed page on the primary, a standby, a
# base backup taken across the share, and after a crash.

use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::RecursiveCopy;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('preserve_skipped');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('preserve_skipped_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.scratch (a int);
	INSERT INTO cow.scratch VALUES (1);
	CREATE TABLE cow.k (a int PRIMARY KEY, b text);
	INSERT INTO cow.k SELECT g, repeat('k', 200) FROM generate_series(1, 100) g;
});
my $k = sql(q{SELECT pg_relation_filenode('cow.k')});
is(sql(qq{SELECT count(*) FROM test_bmap_map($k)}), '3', 'three pages');
my $kmap = sql(qq{SELECT string_agg(phys::text, ',' ORDER BY blkno) FROM test_bmap_map($k)});

my $backup = $node->background_psql('postgres');
$backup->query_safe(q{SELECT pg_backup_start('preserve_skipped', true)});
# after the backup's redo point: later changes of these pages carry no image
sql(q{DELETE FROM cow.k WHERE a BETWEEN 2 AND 6});

my $reader = $node->background_psql('postgres', on_error_stop => 0);
$reader->query_safe(q{SET plan_cache_mode = force_generic_plan;
	PREPARE l AS SELECT a FROM cow.k WHERE a = 100 FOR UPDATE;
	BEGIN; EXECUTE l; COMMIT});
sql(q{SELECT test_bmap_share((SELECT pg_relation_filenode('cow.scratch')))});
sql(q{SELECT pg_stat_reset_shared('prolly')});

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
sql(q{SELECT injection_points_detach('bmap-preserve-page')});
sql(q{SELECT injection_points_wakeup('bmap-preserve-page')});
$node->wait_for_event('client backend', 'bmap-preserve-restart');

# In the window: page 0 (logged already) changed in place by a row lock
# and by a prune.
my $before0 = sql(qq{SELECT test_bmap_rows_digest($k)});
my $locker = $node->background_psql('postgres');
$locker->query_safe(q{BEGIN; SELECT a FROM cow.k WHERE a = 1 FOR UPDATE});
sql(q{SELECT count(*) FROM cow.k WHERE a < 30});   # prune page 0
my $after0 = sql(qq{SELECT test_bmap_rows_digest($k)});
isnt($after0, $before0, 'page 0 changed in the window');
is(sql(qq{SELECT string_agg(phys::text, ',' ORDER BY blkno) FROM test_bmap_map($k)}),
	$kmap, 'in place: same map');

my $copied = $node->backup_dir . '/skipped';
PostgreSQL::Test::RecursiveCopy::copypath(
	$node->data_dir, $copied,
	filterfn => sub {
		my $src = shift;
		return $src ne 'postmaster.pid' && $src ne 'postmaster.opts'
		  && $src !~ /^pg_wal\// && $src ne 'pg_wal';
	});

sql(q{SELECT injection_points_detach('bmap-preserve-restart')});
sql(q{SELECT injection_points_wakeup('bmap-preserve-restart')});
my $made = $op->query_safe(q{SELECT :'made'});
$reader->quit;
$locker->query_safe('COMMIT');
$locker->quit;
# (page 0 logged again, as changed since its image; page 1 skipped)
ok($node->poll_query_until('postgres', q{SELECT value = 4 FROM pg_stat_prolly
	WHERE name = 'bmap_pages_preserved'}), 'page 0 logged again, page 1 not');

my $src = sql(qq{SELECT test_bmap_rows_digest($k)});
my $cp = sql(qq{SELECT test_bmap_rows_digest($made)});
is($cp, $src, 'primary: copy = source');
is($cp, $after0, 'primary: copy has the in-window change');

# source changes after the share: copy-on-write, copy keeps its pages
sql(q{UPDATE cow.k SET b = 'x' WHERE a = 1});
my $src2 = sql(qq{SELECT test_bmap_rows_digest($k)});

$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($made)}), $cp,
	'standby: copy');
is($standby->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($k)}), $src2,
	'standby: source');

my ($label) = $backup->query_safe(q{SELECT labelfile FROM pg_backup_stop(false)});
$backup->quit;
$op->quit;
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
my $restored = PostgreSQL::Test::Cluster->new('preserve_skipped_restored');
$restored->init_from_backup($node, 'skipped');
$restored->start;
is($restored->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($made)}), $cp,
	'backup: copy');
is($restored->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($k)}), $src2,
	'backup: source');
$restored->stop;

$node->stop('immediate');
$node->start;
is(sql(qq{SELECT test_bmap_rows_digest($made)}), $cp, 'crash: copy');
is(sql(qq{SELECT test_bmap_rows_digest($k)}), $src2, 'crash: source');
$node->stop;
$standby->stop;
done_testing();
