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
# for it, and passes by the segments they are on (a freer would find their
# chunks moved already), but compacts the rest, frees kept or not
my $snap = $node->background_psql('postgres');
$snap->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$snap->query_safe('SELECT 1');
my $offset = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM ci');
$node->wait_for_log(qr/chunk freer freed \d+ chunks of chunk index \d+, looking in \d+ buckets marked, and moved [1-9]/, $offset);
$offset = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM ci');
like(slurp_file($node->logfile, $offset),
	qr/a freer to start: \d+ buckets of chunks unheld, [1-9]\d* segments to compact, \d+ to evacuate, [1-9]\d* passed by for snapshots/,
	'a snapshot holding entries moved: their segments passed by, the rest compacted');
$node->wait_for_log(qr/chunk freer freed \d+ chunks of chunk index \d+, looking in \d+ buckets marked, and moved/, $offset);
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
	$node->wait_for_log(qr/chunk freer freed \d+ chunks of chunk index \d+, looking in \d+ buckets marked, and moved/, $offset);
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

# a freer to compact, leaving a bucket for a transaction running there,
# stopped by an error as it compacts (before it waits): the transaction is
# left to the next freer, which waits on it, and frees its chunk as it ends
SKIP:
{
	skip 'injection points not supported by this build', 2
		if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', q{
		CREATE EXTENSION injection_points;
		SELECT count(*) FROM generate_series(1, 2000) g,
			LATERAL test_chunk_put('cl', 'ci', h(blob('c' || g)), blob('c' || g));
		SELECT count(*) FROM generate_series(2, 2000, 2) g,
			LATERAL test_chunk_ref_root('ci', h(blob('c' || g)));
		SELECT sum(test_chunk_free('cl', 'ci', h(blob('c' || g)))) FROM generate_series(1, 2000, 2) g;
		SELECT test_chunk_put('cl', 'ci', h(blob('x')), blob('x'));
		SELECT test_chunk_ref_root('ci', h(blob('x')));
	});
	# (a transaction taking a root of x, running; another letting go of the
	# root x had, x's bucket marked, its commit starting no freer)
	my $t = $node->background_psql('postgres');
	$t->query_safe(q{BEGIN; SELECT test_chunk_ref_root('ci', h(blob('x')))});
	$node->safe_psql('postgres', q{SET chunk_free_in_background = off;
		SELECT test_chunk_unref_root('ci', h(blob('x')))});
	$node->safe_psql('postgres', q{SELECT injection_points_attach('chunk-freer-step', 'error')});
	my $offset = -s $node->logfile;
	$node->safe_psql('postgres', 'VACUUM ci');
	$node->wait_for_log(qr/error triggered for injection point chunk-freer-step/, $offset);
	$node->safe_psql('postgres', q{SELECT injection_points_detach('chunk-freer-step')});
	$t->query_safe('ROLLBACK');
	$t->quit;
	ok($node->poll_query_until('postgres', q{SELECT NOT test_chunk_has('cl', 'ci', h(blob('x')))}),
		'the compacting freer stopped: the transaction it left ended, its chunk freed by the next');
	is($node->safe_psql('postgres', "SELECT ok FROM test_chunk_check('cl', 'ci')"), 't', 'the audit clean');
}

done_testing();
