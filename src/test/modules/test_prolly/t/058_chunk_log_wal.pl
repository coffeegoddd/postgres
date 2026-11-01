# Copyright (c) 2026, PostgreSQL Global Development Group

# The chunk log's own WAL records, replayed: with wal_consistency_checking
# on, every page a standby and crash recovery replay is the primary's.
# Appends that begin, go on in, and span pages, and runs of their own;
# regions sealed; bytes counted dead and pages freed; freed pages made
# afresh and used again; the tail cut off.  A standby promoted has the
# free space map the replay kept, and uses the freed pages again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('log_wal_primary');
# (without checksums or wal_log_hints, whose hint records would carry the
# free space map's pages to the standby now and then)
$primary->init(allows_streaming => 1, extra => ['--no-data-checksums']);
$primary->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_free_in_background = off
wal_consistency_checking = 'ChunkLog'
wal_log_hints = off
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION put(d bytea) RETURNS bool LANGUAGE sql
		AS $$SELECT test_chunk_put('pg_history.chunks', 'pg_history.chunk_index', h(d), d)$$;
	CREATE FUNCTION chunk(i int, len int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT substr(convert_to(repeat(md5(i::text), len / 32 + 1), 'UTF8'), 1, len)$$;
	CREATE TABLE sums (i int, len int);
});
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('log_wal_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

# appends of every shape: small ones packed in pages, ones spanning pages,
# one longer than a region (a run of its own), from several sessions (each
# its own region)
$primary->safe_psql('postgres', q{
	INSERT INTO sums SELECT i, CASE WHEN i % 50 = 0 THEN 12000
		WHEN i = 7 THEN 300000 ELSE 100 + i * 7 END FROM generate_series(1, 400) i;
	SELECT count(*) FILTER (WHERE put(chunk(i, len))) FROM sums WHERE i <= 200;
});
for my $s (0 .. 3)
{
	$primary->safe_psql('postgres',
		"SELECT count(*) FILTER (WHERE put(chunk(i, len))) FROM sums WHERE i > 200 AND i % 4 = $s");
}

# half of them freed: bytes dead, pages freed, the tail cut; then more put,
# into the pages freed
$primary->safe_psql('postgres', q{
	SELECT sum(test_chunk_free('pg_history.chunks', 'pg_history.chunk_index',
		h(chunk(i, len)))) FROM sums WHERE i % 2 = 0 OR i > 300;
	DELETE FROM sums WHERE i % 2 = 0 OR i > 300;
});
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
my $space = q{SELECT pages || ' ' || free_pages FROM test_chunk_log_space('pg_history.chunks')};
my ($pages_before) = split / /, $primary->safe_psql('postgres', $space);
$primary->safe_psql('postgres', q{
	INSERT INTO sums SELECT i, 900 FROM generate_series(1001, 1040) i;
	SELECT count(*) FILTER (WHERE put(chunk(i, len))) FROM sums WHERE i > 1000;
});
my ($pages_after) = split / /, $primary->safe_psql('postgres', $space);
cmp_ok($pages_after, '<=', $pages_before, 'chunks put after the freeing go into pages freed');

my $all = q{SELECT md5(string_agg(encode(test_chunk_get('pg_history.chunks',
	'pg_history.chunk_index', h(chunk(i, len))), 'hex'), ',' ORDER BY i)) FROM sums};
my $want = $primary->safe_psql('postgres', $all);
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $all), $want,
	'the standby replayed the log, every page checked against the primary\'s');
is($standby->safe_psql('postgres', $space), $primary->safe_psql('postgres', $space),
	'and its pages are as the primary\'s');

# crash recovery replays it too
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $all), $want, 'and crash recovery');

# a standby promoted: the free space map its replay kept has the pages
# freed, found from its root.  A standby from a backup whose map has no
# page free (every one used, and VACUUM telling the map's root); then pages
# freed in the log's middle, its tail kept: no truncation replayed, whose
# map vacuum would tell the root too.
$standby->stop;
# (the regions of the sessions before let go of, their pages free counted)
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
my (undef, $nfree) = split / /, $primary->safe_psql('postgres', $space);
$primary->safe_psql('postgres', qq{
	INSERT INTO sums SELECT i, 900 FROM generate_series(4001, 4000 + $nfree * 9 + 20) i;
	SELECT count(*) FILTER (WHERE put(chunk(i, len))) FROM sums WHERE i > 4000;
});
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
is((split / /, $primary->safe_psql('postgres', $space))[1], '0', 'every page in use');
$primary->backup('mid');
my $standby2 = PostgreSQL::Test::Cluster->new('log_wal_standby2');
$standby2->init_from_backup($primary, 'mid', has_streaming => 1);
$standby2->start;
$primary->safe_psql('postgres', q{
	INSERT INTO sums SELECT i, 900 FROM generate_series(3001, 3010) i;
	SELECT count(*) FILTER (WHERE put(chunk(i, len))) FROM sums WHERE i BETWEEN 3001 AND 3010;
});
# (that session's region let go of, its pages never written cut)
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
my $size = q{SELECT pg_relation_size('pg_history.chunks')};
my $size_kept = $primary->safe_psql('postgres', $size);
$primary->safe_psql('postgres', q{
	SELECT sum(test_chunk_free('pg_history.chunks', 'pg_history.chunk_index',
		h(chunk(i, len)))) FROM sums WHERE i > 4000;
	DELETE FROM sums WHERE i > 4000;
});
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index; CREATE TABLE flush ()');
my ($pages, $free) = split / /, $primary->safe_psql('postgres', $space);
is($primary->safe_psql('postgres', $size), $size_kept, 'no tail cut: the pages freed are in the log\'s middle');
$primary->wait_for_replay_catchup($standby2);
$standby2->promote;
($pages, $free) = split / /, $standby2->safe_psql('postgres', $space);
cmp_ok($free, '>', 0, 'the promoted standby has pages freed');
$standby2->safe_psql('postgres', q{
	INSERT INTO sums SELECT i, 900 FROM generate_series(2001, 2005) i;
	SELECT count(*) FILTER (WHERE put(chunk(i, len))) FROM sums WHERE i BETWEEN 2001 AND 2005;
});
my ($pages2, $free2) = split / /, $standby2->safe_psql('postgres', $space);
is($pages2, $pages, 'and puts there use them again, the log not growing');
cmp_ok($free2, '<', $free, 'fewer pages free');

# a pack of small chunks: their bytes written a page at a time, a record
# a page, not one a chunk
$primary->safe_psql('postgres', 'CHECKPOINT');
my $pack_start = $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$primary->safe_psql('postgres', q{
	SELECT test_cs_put_many('pages',
		ARRAY(SELECT h(chunk(i, 100)) FROM generate_series(9001, 10000) i),
		ARRAY(SELECT chunk(i, 100) FROM generate_series(9001, 10000) i));
});
my ($dump) = run_command([ 'pg_waldump', '--rmgr' => 'ChunkLog',
	'--path' => $primary->data_dir . '/pg_wal', '--start' => $pack_start,
	'--end' => $primary->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()') ]);
my $appends = () = $dump =~ /desc: APPEND/g;
# (about 100 kB: some 13 pages)
ok($appends >= 10 && $appends <= 20, "a pack of 1,000 chunks of 100 bytes in $appends append records, about a page each");

$standby2->stop;
$primary->stop;
done_testing();
