# Copyright (c) 2026, PostgreSQL Global Development Group

# A freer finds the chunks to move in one transaction and moves them in
# others: by then a chunk it found may be freed, its entry taken out by
# VACUUM, and its bytes' place used again by another.  Each is moved first,
# the entry seen checked the chunk's still, its bytes read after: one gone
# is passed by, not read where another's bytes are now.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('compaction_stale');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_compact_dead_fraction = 0.4
log_min_messages = debug1
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
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
	-- (and one held at the end, so that the segments freed are in the
	-- log's middle, not cut off with its end)
	SELECT test_chunk_put('cl', 'ci', h(blob('anchor')), blob('anchor'));
	SELECT test_chunk_ref_root('ci', h(blob('anchor')));
});
$node->safe_psql('postgres', q{
	SELECT sum(test_chunk_free('cl', 'ci', h(blob('a' || g)))) FROM generate_series(1, 2000, 2) g;
});
$node->safe_psql('postgres', 'SET chunk_free_in_background = off; VACUUM ci');
$node->safe_psql('postgres', 'SET chunk_free_in_background = off; VACUUM ci');

# a freer stopped after it found the chunks, before it moves any
$node->safe_psql('postgres', q{SELECT injection_points_attach('chunk-freer-step', 'wait')});
my $offset = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM ci');
$node->wait_for_event('chunk freer', 'chunk-freer-step');

# meanwhile every chunk it found freed, the entries taken out, the
# segments free and used again by chunks of others
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(2, 2000, 2) g,
		LATERAL test_chunk_unref_root('ci', h(blob('a' || g)));
	SELECT sum(test_chunk_free('cl', 'ci', h(blob('a' || g)))) FROM generate_series(2, 2000, 2) g;
});
$node->safe_psql('postgres', 'SET chunk_free_in_background = off; VACUUM ci');
$node->safe_psql('postgres', 'SET chunk_free_in_background = off; VACUUM ci');
cmp_ok($node->safe_psql('postgres', q{SELECT free FROM test_chunk_log_segments('cl', 0.4)}),
	'>', 5, 'the segments the chunks found were on free');
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(1, 1500) g,
		LATERAL test_chunk_put('cl', 'ci', h(blob('b' || g)), blob('b' || g));
	SELECT count(*) FROM generate_series(1, 1500) g,
		LATERAL test_chunk_ref_root('ci', h(blob('b' || g)));
});

# the freer on: every chunk it found gone, none moved, no bytes of
# another's taken for one of them
$node->safe_psql('postgres', q{
	SELECT injection_points_detach('chunk-freer-step');
	SELECT injection_points_wakeup('chunk-freer-step');
});
$node->wait_for_log(qr/chunk freer freed \d+ chunks of chunk index \d+, and moved/, $offset);
my $log = slurp_file($node->logfile, $offset);
unlike($log, qr/does not match its hash/, 'no chunk read where another\'s bytes are');
like($log, qr/chunk freer freed \d+ chunks of chunk index \d+, and moved [01] /,
	'none of them moved (but the one held at the end, maybe)');
is( $node->safe_psql('postgres', q{
		SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob('b' || g))) = blob('b' || g))
		FROM generate_series(1, 1500) g}),
	'1500', 'the others\' chunks read back');
is($node->safe_psql('postgres', "SELECT ok FROM test_chunk_check('cl', 'ci')"), 't', 'the audit clean');

$node->stop;
done_testing();
