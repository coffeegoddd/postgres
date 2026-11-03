# Copyright (c) 2026, PostgreSQL Global Development Group

# Compaction in a log of lanes moves chunks into the freer's own lane: room
# free in lanes others hold is none of its, so with every lane there having
# segments free, the freer (its lane new, empty) still grows its own by a
# segment to move into, and moves chunks.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('lane_compaction');
# (the history store holds raw bytes here: initdb makes it with the leaf
# format, which references no chunk)
$node->init(extra => ['-c', 'chunk_format=leaf']);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
log_min_messages = debug1
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE cl () USING prolly_chunks;
	CREATE TABLE ci () USING prolly_chunk_index;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION blob(tag text) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(rpad(tag, 2000, '.'), 'UTF8')$$;
});

# two writers, each in a lane of its own, held as they stay
my @w;
for my $tag ('a', 'b')
{
	my $s = $node->background_psql('postgres');
	$s->query_safe(qq{SELECT count(*) FROM generate_series(1, 1000) g,
		LATERAL test_chunk_put('cl', 'ci', h(blob('$tag' || g)), blob('$tag' || g))});
	push @w, $s;
}
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_class WHERE relname = 'cl_lane_1'}),
	'1', 'the second writer had a lane of its own');

# in each lane: its first chunks all freed (segments free), the rest every
# other one (segments half dead, to compact)
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(402, 1000, 2) g, unnest(ARRAY['a', 'b']) t,
		LATERAL test_chunk_ref_root('ci', h(blob(t || g)));
	SELECT sum(test_chunk_free('cl', 'ci', h(blob(t || g))))
		FROM unnest(ARRAY['a', 'b']) t, generate_series(1, 1000) g WHERE g <= 400 OR g % 2 = 1;
});
is( $node->safe_psql('postgres', q{
		SET chunk_free_in_background = off; VACUUM ci;
		SELECT free > 0 AND compactable > 0 AND worth FROM test_chunk_log_segments('cl', 0.4)}),
	't', 'segments free, and segments to compact');

# the freer's VACUUM: its lane (every other held) new, with no room free
my $offset = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM ci');
$node->wait_for_log(qr/chunk freer freed \d+ chunks of chunk index \d+, looking in \d+ buckets marked, and moved [1-9]/, $offset);
pass('the freer moved chunks into its own lane');
is( $node->safe_psql('postgres', q{
		SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob(t || g))) IS DISTINCT FROM blob(t || g))
		FROM unnest(ARRAY['a', 'b']) t, generate_series(402, 1000, 2) g}),
	'0', 'every chunk held reads back');

$_->quit for @w;
$node->stop;
done_testing();
