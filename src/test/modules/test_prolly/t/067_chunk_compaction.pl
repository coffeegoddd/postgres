# Copyright (c) 2026, PostgreSQL Global Development Group

# Compaction in the background: the log's space is handed out and taken
# back a segment at a time.  VACUUM of a chunk index whose log has segments
# a fraction of whose bytes are dead (chunk_compact_dead_fraction) starts a
# freer that moves their live chunks, packed, into segments free (or, none
# free, one new), and the live chunks of the log's last segments into the
# free ones below them; each VACUUM after takes out the entries moved, the
# segments emptied then free, and cuts the free end off.  Round by round the
# log is compacted to about its live size; its chunks read back, on a hot
# standby too, and segments freed are used again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('compaction');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_compact_dead_fraction = 0.4
log_min_messages = debug1
});
$node->start;
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('compaction_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE cl () USING prolly_chunks;
	CREATE TABLE ci () USING prolly_chunk_index;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION blob(tag text) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(rpad(tag, 2000, '.'), 'UTF8')$$;
	SELECT count(*) FROM generate_series(1, 2000) g,
		LATERAL test_chunk_put('cl', 'ci', h(blob('a' || g)), blob('a' || g));
	SELECT count(*) FROM generate_series(2, 2000, 2) g,
		LATERAL test_chunk_ref_root('ci', h(blob('a' || g)));
});
# (every other one freed: each segment about half dead)
$node->safe_psql('postgres', q{
	SELECT sum(test_chunk_free('cl', 'ci', h(blob('a' || g)))) FROM generate_series(1, 2000, 2) g;
});
my $size0 = $node->safe_psql('postgres', "SELECT pg_relation_size('cl')");
cmp_ok($node->safe_psql('postgres',
		"SET chunk_free_in_background = off; VACUUM ci; SELECT (CASE WHEN worth THEN compactable ELSE 0 END) + evacuable FROM test_chunk_log_segments('cl', 0.4)"),
	'>', 10, 'nearly every segment to compact');

# a snapshot held as a freer moves chunks: VACUUM keeps the entries moved
# for it, and starts no freer while it does (one would find the chunks of
# the segments to compact moved already, and move nothing)
my $snap = $node->background_psql('postgres');
$snap->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$snap->query_safe('SELECT 1');
my $offset = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM ci');
$node->wait_for_log(qr/chunk freer freed \d+ chunks of chunk index \d+, and moved [1-9]/, $offset);
$offset = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM ci');
sleep(1);
unlike(slurp_file($node->logfile, $offset), qr/chunk freer freed/,
	'no freer while a snapshot holds the entries moved');
$snap->query_safe('COMMIT');
$snap->quit;

# VACUUM, the freer's moves, again, until nothing is left to compact: a
# VACUUM with no freer takes out the entries moved, and the next starts
# one, waited for to its end
my $rounds = 0;
my $left;
for (1 .. 30)
{
	$node->safe_psql('postgres', 'SET chunk_free_in_background = off; VACUUM ci');
	$left = $node->safe_psql('postgres', "SELECT (CASE WHEN worth THEN compactable ELSE 0 END) + evacuable FROM test_chunk_log_segments('cl', 0.4)");
	last if $left == 0;
	my $offset = -s $node->logfile;
	$node->safe_psql('postgres', 'VACUUM ci');
	$node->wait_for_log(qr/chunk freer freed \d+ chunks of chunk index \d+, and moved/, $offset);
	$rounds++;
}
is($left, 0, "the log compacted, in $rounds rounds");
my $size1 = $node->safe_psql('postgres', "SELECT pg_relation_size('cl')");
cmp_ok($size1, '<', $size0 * 0.65, 'its end cut off: the log near its live size, half what was written');

is( $node->safe_psql('postgres', q{
		SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob('a' || g))) = blob('a' || g))
		FROM generate_series(2, 2000, 2) g}),
	'1000', 'every chunk kept read back');
is($node->safe_psql('postgres', "SELECT ok FROM test_chunk_check('cl', 'ci')"),
	't', 'the audit clean');

$node->wait_for_replay_catchup($standby);
is( $standby->safe_psql('postgres', q{
		SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob('a' || g))) = blob('a' || g))
		FROM generate_series(2, 2000, 2) g}),
	'1000', 'and on the standby');

# the segments freed used again: 1,000 more, the log no bigger than it was
# written (but its last segment)
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(1, 1000) g,
		LATERAL test_chunk_put('cl', 'ci', h(blob('b' || g)), blob('b' || g));
});
cmp_ok($node->safe_psql('postgres', "SELECT pg_relation_size('cl')"),
	'<=', $size0 + 33 * 8192, 'segments freed used again');
is($node->safe_psql('postgres', "SELECT ok, chunks FROM test_chunk_check('cl', 'ci')"),
	't|2000', 'the audit clean, every chunk there');

done_testing();
