# Copyright (c) 2026, PostgreSQL Global Development Group

# The history store needs nothing of backups and recovery beyond what any
# relation has: a full base backup and an incremental one combined, each
# recovered to a restore point (PITR, from the WAL archive), have the
# store as it was there; pg_verifybackup accepts the backups and
# pg_checksums the store's pages; a crash after a checkpoint replays from
# it; and pg_rewind brings an old primary that went on writing back to
# the promoted standby's store.

use strict;
use warnings FATAL => 'all';
use File::Copy qw(copy);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('backups_primary');
$primary->init(allows_streaming => 1, has_archiving => 1);
$primary->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_free_in_background = off
summarize_wal = on
# (the old primary's WAL kept back to the divergence, for pg_rewind)
wal_keep_size = '1GB'
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	-- chunks of every size: packed in pages, spanning them, some a run of
	-- their own
	CREATE FUNCTION chunk(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT CASE WHEN i % 500 = 0 THEN decode(repeat(md5(i::text), 20000), 'hex')
			ELSE decode(repeat(md5(i::text), 1 + i % 50), 'hex') END$$;
	CREATE FUNCTION h(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(chunk(i)), 1, 20)';
	CREATE FUNCTION put(i int) RETURNS bool LANGUAGE sql
		AS $$SELECT test_chunk_put('pg_history.chunks', 'pg_history.chunk_index', h(i), chunk(i))$$;
	CREATE FUNCTION free(i int) RETURNS int8 LANGUAGE sql
		AS $$SELECT test_chunk_free('pg_history.chunks', 'pg_history.chunk_index', h(i))$$;
	-- every chunk's bytes, or its absence, and every pointer
	CREATE FUNCTION store_state() RETURNS text LANGUAGE sql
		AS $$SELECT md5(string_agg(coalesce(md5(test_chunk_get('pg_history.chunks',
				'pg_history.chunk_index', h(i))), '-'), ',' ORDER BY i)) || ' ' ||
			coalesce((SELECT string_agg(name || '=' || encode(hash, 'hex'), ',' ORDER BY name)
				FROM pg_history.pointers), '')
			FROM generate_series(1, 8000) i$$;
});
my $state = 'SELECT store_state()';
my $audit = q{SELECT ok FROM test_chunk_check('pg_history.chunks', 'pg_history.chunk_index')};

# A: chunks, and a pointer, checkpointed; the full backup
$primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE put(i)) FROM generate_series(1, 2000) i;
	INSERT INTO pg_history.pointers VALUES ('main', h(1));
	CHECKPOINT;
});
$primary->backup('full');
my $full = $primary->backup_dir . '/full';
command_ok([ 'pg_verifybackup', $full ], 'pg_verifybackup accepts a full backup of the store');

# B: more chunks; some freed (bytes dead, pages freed, maybe the tail cut);
# the pointer moved; the incremental backup; a restore point
$primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE put(i)) FROM generate_series(2001, 4000) i;
	SELECT sum(free(i)) FROM generate_series(2, 2000) i WHERE i % 7 = 0;
	VACUUM pg_history.chunk_index;
	SELECT count(*) FILTER (WHERE put(i)) FROM generate_series(4001, 4200) i;
	UPDATE pg_history.pointers SET hash = h(2001);
});
$primary->command_ok(
	[
		'pg_basebackup', '--no-sync',
		'--pgdata' => $primary->backup_dir . '/incr',
		'--checkpoint' => 'fast',
		'--incremental' => "$full/backup_manifest"
	],
	'an incremental backup of the store');
my $combined = $primary->backup_dir . '/combined';
command_ok([ 'pg_combinebackup', '--no-sync', '--output' => $combined, $full,
	$primary->backup_dir . '/incr' ], 'combined with the full one');
command_ok([ 'pg_verifybackup', '--no-parse-wal', $combined ],
	'pg_verifybackup accepts the combined backup');
my $state_b = $primary->safe_psql('postgres', $state);
$primary->safe_psql('postgres', "SELECT pg_create_restore_point('after_b')");

# C: past the restore point; its WAL archived
$primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE put(i)) FROM generate_series(4201, 5000) i;
	UPDATE pg_history.pointers SET hash = h(4201);
});
my $lsn = $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$primary->safe_psql('postgres', 'SELECT txid_current(); SELECT pg_switch_wal()');
$primary->poll_query_until('postgres',
	"SELECT pg_walfile_name('$lsn') <= last_archived_wal FROM pg_stat_archiver")
  or die 'timed out waiting for the WAL to be archived';
isnt($primary->safe_psql('postgres', $state), $state_b, 'the store went on past the restore point');

# PITR to the restore point, from the full backup and from the incremental
for my $from ([ 'full', 'pitr_full' ], [ 'incr', 'pitr_incr', ['full'] ])
{
	my ($backup, $name, $prior) = @$from;
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init_from_backup($primary, $backup, standby => 1, has_restoring => 1,
		$prior ? (combine_with_prior => $prior) : ());
	$node->append_conf('postgresql.conf', qq{
recovery_target_name = 'after_b'
recovery_target_action = 'promote'
archive_mode = off
});
	$node->start;
	$node->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	  or die "timed out waiting for $name to recover";
	is($node->safe_psql('postgres', $state), $state_b,
		"recovered to the restore point from the $backup backup, the store as it was there");
	is($node->safe_psql('postgres', $audit), 't', 'its audit clean');
	$node->stop;
}

# a crash after a checkpoint replays from it; then pg_checksums
$primary->safe_psql('postgres', q{
	CHECKPOINT;
	SELECT count(*) FILTER (WHERE put(i)) FROM generate_series(5001, 5500) i;
	SELECT sum(free(i)) FROM generate_series(4001, 4200) i;
	VACUUM pg_history.chunk_index;
});
my $want = $primary->safe_psql('postgres', $state);
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $state), $want, 'a crash after a checkpoint loses nothing');
$primary->stop;
command_ok([ 'pg_checksums', '--check', '--pgdata' => $primary->data_dir ],
	'pg_checksums accepts the store\'s pages');
$primary->start;

# pg_rewind: a standby promoted, the old primary writing on, rewound
$primary->backup('for_standby');
my $standby = PostgreSQL::Test::Cluster->new('backups_standby');
$standby->init_from_backup($primary, 'for_standby', has_streaming => 1);
$standby->start;
$primary->wait_for_replay_catchup($standby);
$standby->promote;
$primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE put(i)) FROM generate_series(6001, 6300) i;
	SELECT sum(free(i)) FROM generate_series(5001, 5100) i;
	VACUUM pg_history.chunk_index;
	UPDATE pg_history.pointers SET hash = h(6001);
	INSERT INTO pg_history.pointers VALUES ('old_only', h(6002));
	CHECKPOINT;
});
$standby->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE put(i)) FROM generate_series(7001, 7300) i;
	UPDATE pg_history.pointers SET hash = h(7001);
	CHECKPOINT;
});
my $new_state = $standby->safe_psql('postgres', $state);
$primary->stop;

# (pg_rewind copies the source's configuration: the target's kept)
my $conf = $primary->data_dir . '/postgresql.conf';
my $kept = PostgreSQL::Test::Utils::tempdir . '/postgresql.conf';
copy($conf, $kept) or die "copy: $!";
command_ok(
	[
		'pg_rewind',
		'--target-pgdata' => $primary->data_dir,
		'--source-server' => $standby->connstr('postgres'),
		'--write-recovery-conf'
	],
	'pg_rewind of the old primary');
copy($kept, $conf) or die "copy: $!";
$primary->start;
$standby->wait_for_replay_catchup($primary);
is($primary->safe_psql('postgres', $state), $new_state,
	'rewound and following the new primary, the old primary has its store, its own writes gone');

$primary->stop;
$standby->stop;
done_testing();
