# Copyright (c) 2026, PostgreSQL Global Development Group

# By default (block_map_copy_images = on) a copy of a shared page logs the
# page's image, even where the WAL has one since the redo pointer, so no
# recovery needs the old block: an online backup taken on a standby while
# the primary copies shared pages, and changes the blocks they were copied
# from, stops and restores as it does for any relation.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::RecursiveCopy;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('copy_images');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 1GB
wal_keep_size = 1GB
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_walinspect;
});
$primary->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('copy_images_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
checkpoint_timeout = 1h
max_wal_size = 1GB
});
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }

sub rows
{
	my ($node, $table) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT md5(string_agg($table\::text, ',' ORDER BY a)) FROM $table});
}

is(sql(q{SHOW block_map_copy_images}), 'on', 'copies log images by default');

# A table whose every page is changed after a checkpoint (its images are in
# the WAL since the redo pointer), then shared.
sql(q{
	CREATE TABLE t (a int, b text) WITH (fillfactor = 50);
	INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 40 * 17) g;
	CHECKPOINT;
	UPDATE t SET a = a WHERE a % 17 = 1;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
my $pages = sql(q{SELECT pg_relation_size('t') / 8192});
my $copy = sql(qq{SELECT test_bmap_share($t)});
$primary->wait_for_replay_catchup($standby);

# A backup of the standby begins; the primary copies every page, each copy
# with its image, then changes the blocks they were copied from.
my $sbackup = $standby->background_psql('postgres', on_error_stop => 0);
$sbackup->query_safe(q{SET client_min_messages = warning});
$sbackup->query_safe(q{SELECT pg_backup_start('copy_images', true)});
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
sql(q{UPDATE t SET b = 'changed'});
sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
my ($remaps, $images) = split /\|/, sql(qq{
	SELECT count(*), count(*) FILTER (WHERE block_fpi_length > 0)
	  FROM pg_get_wal_block_info('$start', pg_current_wal_lsn())
	 WHERE resource_manager = 'BlockMap' AND record_type = 'REMAP'
	   AND relfilenode = $t AND relforknumber = 0});
is($remaps, $pages, 'each of the table\'s pages is copied');
is($images, $pages, 'and every copy logs the page\'s image');
sql(q{CHECKPOINT});
sql(qq{SELECT test_bmap_init_block($copy, b) FROM generate_series(0, $pages - 1) b});
$primary->wait_for_replay_catchup($standby);

# (the backup's files, copied now, after the old blocks changed)
my $dir = $standby->backup_dir . '/across_copies';
PostgreSQL::Test::RecursiveCopy::copypath($standby->data_dir, $dir,
	filterfn => sub { return $_[0] ne 'postmaster.pid'; });
my $label = $sbackup->query(q{SELECT labelfile FROM pg_backup_stop()});
is($sbackup->{stderr}, '', 'the standby\'s backup across the copies stops');
$sbackup->quit;

PostgreSQL::Test::Utils::append_to_file("$dir/backup_label", $label);
my $restored = PostgreSQL::Test::Cluster->new('copy_images_restored');
$restored->init_from_backup($standby, 'across_copies');
ok($restored->start(fail_ok => 1), 'and the backup restores');
is(rows($restored, 't'), rows($primary, 't'), 'reading the table\'s rows');
$restored->stop('immediate') if defined $restored->{_pid};

done_testing();
