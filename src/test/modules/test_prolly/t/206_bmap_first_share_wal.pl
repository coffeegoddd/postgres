# Copyright (c) 2026, PostgreSQL Global Development Group

# A share's WAL is O(1) in the source's size while no online backup runs:
# no page images of the blocks it shares.  Crash recovery and a standby
# need none (the share's flush, and its replay's, leave the blocks durable
# first), nor does a backup that starts after the share.  A backup running
# on the primary gets the images; one that starts during the share fails
# the share; one taken from a standby that replays a share meanwhile fails.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('first_share');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	 CREATE EXTENSION pg_walinspect');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('first_share_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub ssql { return $standby->safe_psql('postgres', $_[0]); }

my $preserved =
  q{SELECT value FROM pg_stat_prolly WHERE name = 'bmap_pages_preserved'};

# A share of rel: its copy, the WAL it wrote, its page images, and the
# PRESERVE records in that WAL.
sub share
{
	my ($rel) = @_;
	sql('SELECT pg_stat_force_next_flush()');
	my $p0 = sql($preserved);
	my $l0 = sql('SELECT pg_current_wal_insert_lsn()');
	my $copy = sql(qq{SELECT test_bmap_share($rel)});
	my $l1 = sql('SELECT pg_current_wal_insert_lsn()');
	sql('SELECT pg_stat_force_next_flush()');
	return (
		$copy,
		sql(qq{SELECT '$l1'::pg_lsn - '$l0'::pg_lsn}),
		sql($preserved) - $p0,
		sql(qq{SELECT count(*) FROM pg_get_wal_records_info('$l0', '$l1')
			WHERE resource_manager = 'BlockMap' AND record_type = 'PRESERVE'}));
}

sql(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 10);
	  INSERT INTO t SELECT g, md5(g::text) FROM generate_series(1, 20000) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
my $blocks = sql(q{SELECT pg_relation_size('t') / 8192});
cmp_ok($blocks, '>', 1000, 'the table has many blocks');
my $digest = sql(qq{SELECT test_bmap_rows_digest($rel)});

# No backup: the first share logs no images, and little WAL.
my ($copy, $wal, $images, $records) = share($rel);
is($images, 0, 'with no backup running, a first share logs no page images');
is($records, 0, 'nor PRESERVE records');
cmp_ok($wal, '<', 64 * 1024, "its WAL is small ($wal bytes for $blocks blocks)");
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $digest, 'the copy has the rows');

# The source then changes every page (each copied first) and its map
# reaches disk, so redo would skip its records from before the share.
sql(q{UPDATE t SET b = 'changed'; CHECKPOINT});
my $changed = sql(qq{SELECT test_bmap_rows_digest($rel)});
$node->wait_for_replay_catchup($standby);
is(ssql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'a standby\'s copy has the rows');

# A backup taken now, and restored with WAL past more changes, has both.
$node->backup('after_share');
sql(q{UPDATE t SET b = 'again' WHERE a % 2 = 0});
my $again = sql(qq{SELECT test_bmap_rows_digest($rel)});
sql('SELECT pg_switch_wal()');
my $restored = PostgreSQL::Test::Cluster->new('first_share_restored');
$restored->init_from_backup($node, 'after_share', has_streaming => 1);
$restored->start;
$node->wait_for_replay_catchup($restored);
is($restored->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($copy)}),
	$digest, 'a backup taken after the share restores the copy');
is($restored->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($rel)}),
	$again, 'and the source');
$restored->stop;

# Crash recovery and a standby's restart, the share's records replayed.
my $copy2;
($copy2) = share($rel);
sql(q{UPDATE t SET b = 'third'});
$node->wait_for_replay_catchup($standby);
$node->stop('immediate');
$node->start;
is(sql(qq{SELECT test_bmap_rows_digest($copy2)}), $again,
	'a copy survives a crash, its source changed since');
$standby->stop('immediate');
$standby->start;
$node->wait_for_replay_catchup($standby);
is(ssql(qq{SELECT test_bmap_rows_digest($copy2)}), $again,
	'and a standby\'s restart');

# A backup running on the primary: the share logs its images.
my $bk = $node->background_psql('postgres');
$bk->query_safe(q{SELECT pg_backup_start('running', true)});
sql(q{CREATE TABLE u (a int, b text) WITH (fillfactor = 10);
	  INSERT INTO u SELECT g, md5(g::text) FROM generate_series(1, 2000) g});
my $urel = sql(q{SELECT pg_relation_filenode('u')});
my $ublocks = sql(q{SELECT pg_relation_size('u') / 8192});
my ($ucopy, $uwal, $uimages, $urecords) = share($urel);
is($uimages, $ublocks, 'with a backup running, a share logs an image of each block');
is($urecords, $ublocks, 'as PRESERVE records');
$bk->query_safe(q{SELECT * FROM pg_backup_stop(false)});

# A backup that starts while a share writes its blocks fails the share.
my $sh = $node->background_psql('postgres', on_error_stop => 0);
$sh->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-share-flushed', 'wait')});
$sh->query_until(qr/sharing/, qq{\\echo sharing
	SELECT test_bmap_share($urel);
	\\echo shared
});
$node->wait_for_event('client backend', 'bmap-share-flushed');
$bk->query_safe(q{SELECT pg_backup_start('meanwhile', true)});
sql(q{SELECT injection_points_wakeup('bmap-share-flushed')});
my $out = $sh->query(q{SELECT 1});
like($sh->{stderr}, qr/an online backup started meanwhile/,
	'a backup begun during a share fails the share');
$sh->quit;
$bk->query_safe(q{SELECT * FROM pg_backup_stop(false)});
$bk->quit;

# A backup taken from the standby fails if a share is replayed meanwhile.
my $sbk = $standby->background_psql('postgres', on_error_stop => 0);
$sbk->query_safe(q{SELECT pg_backup_start('standby', true)});
share($rel);
$node->wait_for_replay_catchup($standby);
$sbk->query(q{SELECT * FROM pg_backup_stop(false)});
like($sbk->{stderr}, qr/share of block-mapped relations' blocks was replayed during online backup/,
	'a standby\'s backup fails when a share is replayed meanwhile');
$sbk->{stderr} = '';
$sbk->query_safe(q{SELECT pg_backup_start('standby2', true)});
$sbk->query_safe(q{SELECT * FROM pg_backup_stop(false)});
is($sbk->{stderr}, '', 'one begun after the replay does not');
$sbk->quit;

$standby->stop;
$node->stop;
done_testing();
