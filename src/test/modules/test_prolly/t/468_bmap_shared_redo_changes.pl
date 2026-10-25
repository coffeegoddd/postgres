# Copyright (c) 2026, PostgreSQL Global Development Group

# A share at wal_level replica logs the source's own pages as images, and
# then, before its record, a row lock and a prune change one of them in
# place (the page found the source's own before the share, so neither asks
# the map).  Redo of the share has the copy read the page as changed, as
# the primary's flush of the source's buffers gives it: after a crash, on a
# standby, and from a base backup taken across the share.

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

my $node = PostgreSQL::Test::Cluster->new('shared_redo_changes');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('shared_redo_changes_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# A page of rows a delete left dead, which a scan prunes.
sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE SCHEMA cow VERSIONED;
});
sql(q{CREATE TABLE cow.k (a int PRIMARY KEY, b text);
	INSERT INTO cow.k SELECT g, repeat('k', 230) FROM generate_series(1, 30) g});
my $k = sql(q{SELECT pg_relation_filenode('cow.k')});
is(sql(qq{SELECT count(*) FROM test_bmap_map($k)}), '1', 'one page');

my $before = sql(qq{SELECT test_bmap_rows_digest($k)});
my $backup = $node->background_psql('postgres');
$backup->query_safe(q{SELECT pg_backup_start('changes', true)});
sql(q{DELETE FROM cow.k WHERE a BETWEEN 2 AND 6});

# The page found the source's own (a row lock prepared it), and a cursor
# whose index scan prunes the page and locks a row of it set up, before the
# share holds the header (planning and a scan's start read the length).
my $reader = $node->background_psql('postgres', on_error_stop => 0);
$reader->query_safe(q{SELECT a FROM cow.k WHERE a = 1 FOR UPDATE});
$reader->query_safe(q{BEGIN;
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off;
	DECLARE c CURSOR FOR SELECT a FROM cow.k WHERE a >= 1 ORDER BY a FOR UPDATE});

# The share stops after logging the page's image, before its record.
my $op = $node->background_psql('postgres', on_error_stop => 0);
$op->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-share-map-page', 'wait')});
$op->query_until(qr/started/,
	qq{\\echo started\nSELECT test_bmap_share($k) AS made \\gset\n});
$node->wait_for_event('client backend', 'bmap-share-map-page');

# Meanwhile the page is locked and pruned in place.
$reader->query_safe(q{FETCH 1 FROM c; COMMIT});

# A base backup's files copied meanwhile, before the share writes the
# page out; the backup ends after the share.
my $copied = $node->backup_dir . '/changes';
PostgreSQL::Test::RecursiveCopy::copypath(
	$node->data_dir, $copied,
	filterfn => sub {
		my $src = shift;
		return $src ne 'postmaster.pid' && $src ne 'postmaster.opts'
		  && $src !~ /^pg_wal\// && $src ne 'pg_wal';
	});
sql(q{SELECT injection_points_detach('bmap-share-map-page')});
sql(q{SELECT injection_points_wakeup('bmap-share-map-page')});
my $made = $op->query_safe(q{SELECT :'made'});
$reader->quit;
my $after = sql(qq{SELECT test_bmap_rows_digest($k)});
isnt($after, $before, 'the page changed in place');
is(sql(qq{SELECT test_bmap_rows_digest($made)}), $after,
	'the copy has the page as changed');

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

$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($made)}), $after,
	'a standby\'s copy has the page as changed');
is($standby->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($k)}), $after,
	'as does its source');

my $restored = PostgreSQL::Test::Cluster->new('shared_redo_changes_restored');
$restored->init_from_backup($node, 'changes');
$restored->start;
is($restored->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($made)}), $after,
	'so does a base backup\'s');
$restored->stop;

$node->stop('immediate');
$node->start;
is(sql(qq{SELECT test_bmap_rows_digest($made)}), $after, 'and the copy after a crash');
is(sql(qq{SELECT test_bmap_rows_digest($k)}), $after, 'as the source');
is(sql(q{SELECT count(*) FROM cow.k}), '25', 'the rows');

# A snapshot the same way: its own page logged as an image, then locked and
# pruned in place before the snapshot's record.
sql(q{CREATE TABLE cow.s (a int PRIMARY KEY, b text);
	INSERT INTO cow.s SELECT g, repeat('s', 230) FROM generate_series(1, 30) g});
sql(q{DELETE FROM cow.s WHERE a BETWEEN 2 AND 6});
my $srel = sql(q{SELECT pg_relation_filenode('cow.s')});
$reader = $node->background_psql('postgres', on_error_stop => 0);
$reader->query_safe(q{SELECT a FROM cow.s WHERE a = 1 FOR UPDATE});
$reader->query_safe(q{BEGIN;
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off;
	DECLARE c CURSOR FOR SELECT a FROM cow.s WHERE a >= 1 ORDER BY a FOR UPDATE});
$op = $node->background_psql('postgres', on_error_stop => 0);
$op->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-snapshot-preserved', 'wait')});
$op->query_until(qr/started/,
	qq{\\echo started\nSELECT test_bmap_snapshot_take($srel) AS id \\gset\n});
$node->wait_for_event('client backend', 'bmap-snapshot-preserved');
$reader->query_safe(q{FETCH 1 FROM c; COMMIT});
sql(q{SELECT injection_points_detach('bmap-snapshot-preserved')});
sql(q{SELECT injection_points_wakeup('bmap-snapshot-preserved')});
my $id = $op->query_safe(q{SELECT :'id'});
$op->quit;
$reader->quit;
my $kept = sql(qq{SELECT test_bmap_share_snapshot($srel, $id)});
my $skept = sql(qq{SELECT test_bmap_rows_digest($kept)});
is($skept, sql(qq{SELECT test_bmap_rows_digest($srel)}),
	'a relation made of the snapshot has the page as changed');

# The standby, promoted, makes a relation of its snapshot too.
$node->wait_for_replay_catchup($standby);
$standby->promote;
my $skept2 = $standby->safe_psql('postgres', qq{SELECT test_bmap_share_snapshot($srel, $id)});
is($standby->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($skept2)}), $skept,
	'so does one a standby makes of its snapshot');

$standby->stop;
$node->stop;
done_testing();
