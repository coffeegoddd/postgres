# Copyright (c) 2026, PostgreSQL Global Development Group

# Freeing in the background: VACUUM of a chunk index starts a chunk freer,
# which frees the chunks nothing holds; the next VACUUM counts their bytes
# dead and cuts the log's free tail off.  Autovacuum, VACUUMing the index
# for its inserts, does the same unasked.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('chunk_freer');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE cl () USING prolly_chunks;
	CREATE TABLE ci () USING prolly_chunk_index;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION blob(tag text) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(rpad(tag, 2000, '.'), 'UTF8')$$;
	-- 10 chunks with a root, then 400 nothing holds (at the log's end)
	SELECT count(test_chunk_put('cl', 'ci', h(blob('rooted ' || g)), blob('rooted ' || g)))
	FROM generate_series(1, 10) g;
	SELECT count(test_chunk_put('cl', 'ci', h(blob('loose ' || g)), blob('loose ' || g)))
	FROM generate_series(1, 400) g;
	SELECT count(test_chunk_ref_root('ci', h(blob('rooted ' || g)))) FROM generate_series(1, 10) g;
});
my $size_before = $node->safe_psql('postgres', q{SELECT pg_relation_size('cl')});

# (the freer gone too: its locks held, the next VACUUM cuts nothing)
my $no_freer = q{SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'chunk freer'};

$node->safe_psql('postgres', 'VACUUM ci');
is($node->safe_psql('postgres', q{SELECT n_ins_since_vacuum FROM pg_stat_user_tables WHERE relname = 'ci'}),
	'0', 'VACUUM of the index is counted: autovacuum starts again from there');
my $loose = q{SELECT count(*) FILTER (WHERE test_chunk_has('cl', 'ci', h(blob('loose ' || g))))
	FROM generate_series(1, 400) g};
ok($node->poll_query_until('postgres', $loose, '0'),
	'a chunk freer frees the chunks nothing holds');
ok($node->poll_query_until('postgres', $no_freer), 'and exits');
is($node->safe_psql('postgres', q{SELECT count(*) FILTER (WHERE test_chunk_has('cl', 'ci', h(blob('rooted ' || g))))
	FROM generate_series(1, 10) g}), '10', 'and leaves those rooted');

# the next VACUUM counts their bytes dead; the free tail is cut off
$node->safe_psql('postgres', 'VACUUM ci');
my $size_after = $node->safe_psql('postgres', q{SELECT pg_relation_size('cl')});
ok($size_after < $size_before, "the log is shorter ($size_before to $size_after bytes)");
is($node->safe_psql('postgres', q{SELECT ok FROM test_chunk_check('cl', 'ci')}), 't',
	'the audit agrees');
is($node->safe_psql('postgres', q{SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob('rooted ' || g))) = blob('rooted ' || g))
	FROM generate_series(1, 10) g}), '10', 'and the rooted chunks read back');

# appends after the cut go on from where it is
$node->safe_psql('postgres', q{SELECT count(test_chunk_put('cl', 'ci', h(blob('after ' || g)), blob('after ' || g)))
	FROM generate_series(1, 50) g});
is($node->safe_psql('postgres', q{SELECT ok FROM test_chunk_check('cl', 'ci')}), 't',
	'puts after the cut, the audit agrees');

# autovacuum, for the index's inserts, starts the freer unasked
$node->append_conf('postgresql.conf', q{
autovacuum = on
autovacuum_naptime = 1s
autovacuum_vacuum_insert_threshold = 5
autovacuum_vacuum_insert_scale_factor = 0
});
$node->reload;
ok($node->poll_query_until('postgres',
		q{SELECT count(*) FILTER (WHERE test_chunk_has('cl', 'ci', h(blob('after ' || g))))
		FROM generate_series(1, 50) g}, '0'),
	'autovacuum and a chunk freer free chunks nothing holds');

# letting go of roots alone has autovacuum free the chunks they held
$node->safe_psql('postgres', q{SELECT count(test_chunk_unref_root('ci', h(blob('rooted ' || g))))
	FROM generate_series(1, 10) g});
ok($node->poll_query_until('postgres',
		q{SELECT count(*) FILTER (WHERE test_chunk_has('cl', 'ci', h(blob('rooted ' || g))))
		FROM generate_series(1, 10) g}, '0'),
	'roots let go of, autovacuum and a freer free their chunks');

$node->stop;
done_testing();
