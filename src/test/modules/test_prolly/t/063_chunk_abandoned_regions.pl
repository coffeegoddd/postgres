# Copyright (c) 2026, PostgreSQL Global Development Group

# A backend's region of a chunk log, the backend gone, is let go of by
# VACUUM: the log's map marks the pages handed to regions, and VACUUM makes
# each one marked full that no live backend's region has.
# - a crash: the regions of every backend, gone with it, let go of;
# - a backend gone during VACUUM, its put's entry put and its bytes not
#   written: that VACUUM leaves its region (the entry, aborted, is the next
#   one's, which counts its bytes dead, and then lets the region go);
# - a log past its first map page's group: its bytes past the map page,
#   regions there let go of, and every page freed, the log cut back to its
#   meta page, map pages and all.
# With wal_consistency_checking, a standby and crash recovery replay it all
# page for page.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('abandoned');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_free_in_background = off
wal_consistency_checking = 'ChunkLog, ChunkIndex'
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION blob(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'L'::bytea || decode(repeat(md5('ab' || i), 20), 'hex')$$;
	-- (longer than a region: one of its own)
	CREATE FUNCTION huge(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'L'::bytea || decode(repeat(md5('ab' || i), 20000), 'hex')$$;
	CREATE TABLE cl () USING prolly_chunks;
	CREATE TABLE ci () USING prolly_chunk_index;
	CREATE TABLE ml () USING prolly_chunks;
	CREATE TABLE mi () USING prolly_chunk_index;
});
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('abandoned_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

my $space = q{SELECT pages || ' ' || full_pages FROM test_chunk_log_space('cl')};
my $audit = q{SELECT ok FROM test_chunk_check('pg_history.chunks', 'pg_history.chunk_index')};

# 1. a crash: a backend's region, its last page part written, lost with it
my $s = $primary->background_psql('postgres');
$s->query_safe(q{SELECT count(*) FILTER (WHERE test_chunk_put('cl', 'ci', h(blob(i)), blob(i))) FROM generate_series(1, 30) i});
my ($pages, $full) = split / /, $s->query_safe($space);
cmp_ok($full, '<', $pages, 'a region\'s last page part written');
$primary->safe_psql('postgres', 'VACUUM ci');
is($primary->safe_psql('postgres', $space), "$pages $full", 'VACUUM leaves a live backend\'s region as it is');
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->stop('immediate');
$s->{run}->finish;
$primary->start;
$primary->safe_psql('postgres', 'VACUUM ci');
is($primary->safe_psql('postgres', $space), "$pages $pages", 'after a crash, VACUUM makes the region\'s last page full');
is($primary->safe_psql('postgres', q{SELECT ok FROM test_chunk_check('cl', 'ci')}), 't', 'the audit clean');

# 2. a backend gone during VACUUM, its put's entry in, its bytes not written
$primary->safe_psql('postgres', q{SELECT (test_cs_put('pages', blob(100))).stored});
my $p = $primary->background_psql('postgres', on_error_stop => 0);
$p->query_safe(q{SELECT injection_points_set_local()});
# (a chunk of its own first: the put's bytes reserved on a page written)
$p->query_safe(q{SELECT (test_cs_put('pages', blob(102))).stored});
$p->query_safe(q{SELECT injection_points_attach('chunk-put-entered', 'wait')});
$p->query_safe(q{SET application_name = 'entered'});
$p->query_until(qr/putting/, "\\echo putting\nSELECT test_cs_put('pages', blob(101));\n");
$primary->wait_for_event('client backend', 'chunk-put-entered');
my $v = $primary->background_psql('postgres');
$v->query_safe(q{SELECT injection_points_set_local()});
$v->query_safe(q{SELECT injection_points_attach('chunk-index-vacuum-sweep', 'wait')});
is($primary->safe_psql('postgres', q{SELECT test_chunk_log_regions('pg_history.chunks')}), '1',
	'the put\'s backend\'s region live, the one before it gone');
$v->query_until(qr/vacuuming/, "\\echo vacuuming\nVACUUM pg_history.chunk_index;\n");
$primary->wait_for_event('client backend', 'chunk-index-vacuum-sweep');
$primary->safe_psql('postgres',
	q{SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'entered'});
# (gone once its region is: it holds none as it exits)
$primary->poll_query_until('postgres',
	q{SELECT test_chunk_log_regions('pg_history.chunks') = 0})
  or die 'the put\'s backend did not exit';
$p->{run}->finish;
$primary->safe_psql('postgres', q{
	SELECT injection_points_detach('chunk-index-vacuum-sweep');
	SELECT injection_points_wakeup('chunk-index-vacuum-sweep');
});
$v->query_safe('SELECT 1');
$v->quit;
# (the next VACUUM takes the entry out, its bytes, never written, counted
# dead, and makes the region's page full past them: none counted twice)
my ($ok, $stderr) = (0, '');
$ok = $primary->psql('postgres', 'VACUUM pg_history.chunk_index', stderr => \$stderr);
is($ok, 0, 'the next VACUUM counts the gone put\'s bytes dead, and lets its region go');
is($stderr, '', 'no byte counted dead twice');
is($primary->safe_psql('postgres', $audit), 't', 'the audit clean');

# 3. a log past its first map page's group (4096 data pages)
$primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE test_chunk_put('ml', 'mi', h(huge(i)), huge(i))) FROM generate_series(1, 110) i;
	SELECT roots FROM test_chunk_ref_root('mi', h(huge(110)));
});
cmp_ok($primary->safe_psql('postgres', q{SELECT pg_relation_size('ml') / current_setting('block_size')::int}),
	'>', 4098, 'the log past its first group');
# (a backend's region past the second map page, it gone)
$primary->safe_psql('postgres', q{SELECT test_chunk_put('ml', 'mi', h(blob(7)), blob(7))});
$primary->safe_psql('postgres', 'VACUUM mi');
is($primary->safe_psql('postgres', q{SELECT pages = full_pages FROM test_chunk_log_space('ml')}), 't',
	'past the second map page, a region gone let go of');
is($primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE test_chunk_get('ml', 'mi', h(huge(i))) = huge(i)) FROM generate_series(1, 110) i}),
	'110', 'the bytes read back, across the map page');
is($primary->safe_psql('postgres', q{SELECT ok FROM test_chunk_check('ml', 'mi')}), 't', 'the audit clean');
$primary->safe_psql('postgres', q{
	SELECT roots FROM test_chunk_unref_root('mi', h(huge(110)));
	SELECT test_chunk_free_unheld('ml', 'mi');
});
$primary->safe_psql('postgres', 'VACUUM mi');
is($primary->safe_psql('postgres', q{SELECT pg_relation_size('ml') = current_setting('block_size')::int}), 't',
	'everything freed, the log cut back to its meta page, map pages and all');

$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', q{SELECT pg_relation_size('cl')}),
	$primary->safe_psql('postgres', q{SELECT pg_relation_size('cl')}),
	'the standby replayed it all, every page checked against the primary\'s');
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $audit), 't', 'and crash recovery');

$standby->stop;
$primary->stop;
done_testing();
