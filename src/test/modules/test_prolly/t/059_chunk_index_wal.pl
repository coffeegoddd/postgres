# Copyright (c) 2026, PostgreSQL Global Development Group

# The chunk index's own WAL records, replayed: with wal_consistency_checking
# on, every page a standby and crash recovery replay is the primary's (but
# for the hint flags each server learns of its own).  Entries added, and
# pages of their own at a chain's end; buckets split; counts changed, and
# changed again in one transaction; chunks freed, and freeings aborted;
# VACUUM freezing, clearing, folding changes in and taking entries out;
# entries removed; aborted ones purged.  A directory page made, of a split
# past the first's buckets.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('index_wal_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_free_in_background = off
wal_consistency_checking = 'ChunkLog, ChunkIndex'
# (the WAL kept, for pg_waldump to read: the checks' images fill segments)
wal_keep_size = '2GB'
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION h(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(int4send(i)), 1, 20)';
	CREATE FUNCTION put(i int) RETURNS bool LANGUAGE sql
		AS $$SELECT test_chunk_put('pg_history.chunks', 'pg_history.chunk_index', h(i), int4send(i))$$;
	CREATE TABLE ids (i int);
	CREATE TABLE flush (x int);
});
my $start_lsn = $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('index_wal_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

# puts: buckets split, chains grow pages
$primary->safe_psql('postgres', q{
	INSERT INTO ids SELECT generate_series(1, 4000);
	SELECT count(*) FILTER (WHERE put(i)) FROM ids;
});
# counts: roots taken, some twice in a transaction, some let go of
$primary->safe_psql('postgres', q{
	BEGIN;
	SELECT count(*) FROM ids, LATERAL test_chunk_ref_root('pg_history.chunk_index', h(i)) WHERE i % 3 = 0;
	SELECT count(*) FROM ids, LATERAL test_chunk_ref_root('pg_history.chunk_index', h(i)) WHERE i % 6 = 0;
	SELECT count(*) FROM ids, LATERAL test_chunk_unref_root('pg_history.chunk_index', h(i)) WHERE i % 12 = 0;
	COMMIT;
});
# frees: committed, and aborted
$primary->safe_psql('postgres', q{
	SELECT sum(test_chunk_free('pg_history.chunks', 'pg_history.chunk_index', h(i)))
		FROM ids WHERE i % 5 = 1;
	DELETE FROM ids WHERE i % 5 = 1;
	BEGIN;
	SELECT sum(test_chunk_free('pg_history.chunks', 'pg_history.chunk_index', h(i)))
		FROM ids WHERE i % 5 = 2;
	ROLLBACK;
});
# VACUUM: entries frozen, freeings cleared, changes folded in, the freed out
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
# a removal; an aborted put, purged by the put of its chunk again
$primary->safe_psql('postgres', q{
	SELECT test_chunk_index_remove('pg_history.chunk_index', h(7));
	DELETE FROM ids WHERE i = 7;
	SELECT sum(test_chunk_free('pg_history.chunks', 'pg_history.chunk_index', h(i)))
		FROM ids WHERE i BETWEEN 5001 AND 5000;
	BEGIN;
	SELECT put(9001);
	ROLLBACK;
	SELECT put(9001);
	INSERT INTO ids VALUES (9001);
});
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');

my $entries = q{SELECT md5(string_agg(e::text, ',' ORDER BY i)) FROM ids,
	LATERAL test_chunk_index_entry('pg_history.chunk_index', h(i)) e};
my $shape = q{SELECT buckets || ' ' || pages || ' ' || entries
	FROM test_chunk_index_shape('pg_history.chunk_index')};
my $want = $primary->safe_psql('postgres', $entries);
my $want_shape = $primary->safe_psql('postgres', $shape);
$primary->safe_psql('postgres', 'INSERT INTO flush VALUES (1)');
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $entries), $want,
	'the standby replayed the index, every page checked against the primary\'s');
is($standby->safe_psql('postgres', $shape), $want_shape, 'its shape the primary\'s');

# every record replayed
my $waldump = sub {
	my ($from) = @_;
	my ($out) = run_command([ 'pg_waldump', '--rmgr' => 'ChunkIndex',
		'--path' => $primary->data_dir . '/pg_wal', '--start' => $from,
		'--end' => $primary->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()') ]);
	return $out;
};
my $out = $waldump->($start_lsn);
my %seen = map { $_ => 1 } ($out =~ /desc: ([A-Z_]+)/g);
is(join(' ', sort keys %seen),
	'ADD_ENTRY ADD_PAGE BIND_LOG FOLD INIT_PAGE KEEP REMOVE_SLOT SET_COUNTS SET_XMAX SPLIT VACUUM',
	'every record of the index\'s is among them');

# crash recovery too
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $entries), $want, 'and crash recovery');

# a directory page: buckets past the first page's, without the consistency
# checks' images (whose WAL would be gigabytes)
my $dir_lsn = $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$primary->safe_psql('postgres', q{
	SET wal_consistency_checking = '';
	INSERT INTO ids SELECT generate_series(100001, 600000);
	SELECT count(*) FILTER (WHERE put(i)) FROM ids WHERE i > 100000;
});
like($waldump->($dir_lsn), qr/desc: INIT_PAGE kind: 2,/,
	'the index made a directory page, of buckets past the first one\'s');
$want = $primary->safe_psql('postgres', $entries);
$primary->safe_psql('postgres', 'INSERT INTO flush VALUES (1)');
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $entries), $want, 'and the standby has every entry the same');
like($standby->safe_psql('postgres', q{SELECT 1}), qr/1/, 'alive');

$standby->stop;
$primary->stop;
done_testing();
