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

# a standby promoted: the free space map its replay kept has the freed pages
$primary->safe_psql('postgres', q{
	SELECT sum(test_chunk_free('pg_history.chunks', 'pg_history.chunk_index',
		h(chunk(i, len)))) FROM sums WHERE i > 1000;
	DELETE FROM sums WHERE i > 1000;
});
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index; CREATE TABLE flush ()');
$primary->wait_for_replay_catchup($standby);
$standby->promote;
my ($pages, $free) = split / /, $standby->safe_psql('postgres', $space);
cmp_ok($free, '>', 0, 'the promoted standby has pages freed');
$standby->safe_psql('postgres', q{
	INSERT INTO sums SELECT i, 900 FROM generate_series(2001, 2005) i;
	SELECT count(*) FILTER (WHERE put(chunk(i, len))) FROM sums WHERE i > 2000;
});
my ($pages2, $free2) = split / /, $standby->safe_psql('postgres', $space);
is($pages2, $pages, 'and puts there use them again, the log not growing');
cmp_ok($free2, '<', $free, 'fewer pages free');

$standby->stop;
$primary->stop;
done_testing();
