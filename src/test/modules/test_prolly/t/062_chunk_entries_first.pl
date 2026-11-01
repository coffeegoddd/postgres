# Copyright (c) 2026, PostgreSQL Global Development Group

# A put's entry is in its index before its bytes are in the log, its
# bytes' place reserved as the entry is put: every byte the log holds for a
# put is an entry's, committed or aborted, and VACUUM counts an aborted
# one's dead, written or not.  So nothing a failed put reserved is lost:
# - a crash between a put's entry and its bytes: the entry, aborted, has
#   its bytes (never written) counted dead by VACUUM;
# - a tail cut taking the pages of an aborted entry's bytes reserved, never
#   written (the entry found running by VACUUM, aborted before the cut):
#   the next VACUUM counts nothing of them, the cut's generation after
#   theirs, and the log does not grow back to count them;
# - everything freed, the log goes back to its meta page: no byte uncounted.
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

my $primary = PostgreSQL::Test::Cluster->new('entries_first');
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
		AS $$SELECT 'L'::bytea || decode(repeat(md5('ef' || i), 20), 'hex')$$;
	-- (longer than a page: never into a page freed, but pages new at the end)
	CREATE FUNCTION big(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'L'::bytea || decode(repeat(md5('ef' || i), 600), 'hex')$$;
});
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('entries_first_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

my $size = q{SELECT pg_relation_size('pg_history.chunks')};
my $audit = q{SELECT ok FROM test_chunk_check('pg_history.chunks', 'pg_history.chunk_index')};

# live chunks, to hold the log's beginning
$primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE (test_cs_put('pages', blob(i))).stored) FROM generate_series(1, 50) i;
});

# 1. a crash between a put's entry and its bytes
my $s = $primary->background_psql('postgres');
$s->query_safe(q{SELECT injection_points_set_local()});
$s->query_safe(q{SELECT injection_points_attach('chunk-put-entered', 'wait')});
$s->query_until(qr/putting/, "\\echo putting\nSELECT test_cs_put('pages', blob(1001));\n");
$primary->wait_for_event('client backend', 'chunk-put-entered');
$primary->stop('immediate');
$s->{run}->finish;
$primary->start;
is($primary->safe_psql('postgres',
		q{SELECT test_cs_has('pages', h(blob(1001)))}), 'f', 'after the crash, the put is not there');
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
is($primary->safe_psql('postgres', $audit), 't', 'VACUUM counted its bytes, never written, dead; the audit clean');

# 2. a tail cut between VACUUM's finding a put running and its entry's end
# (the put's bytes on pages new at the log's end, past them another put's,
# rolled back, so that the cut takes the pages)
my $p = $primary->background_psql('postgres', on_error_stop => 0);
$p->query_safe(q{SELECT injection_points_set_local()});
$p->query_safe(q{SELECT injection_points_attach('chunk-put-entered', 'wait')});
$p->query_safe(q{SET application_name = 'entered'});
$p->query_until(qr/putting/, "\\echo putting\nSELECT test_cs_put('pages', big(2001));\n");
$primary->wait_for_event('client backend', 'chunk-put-entered');
# (another put, past the first's region, rolled back: the log's file made as
# far as that, the first's pages in it never written, then all of it dead)
$primary->safe_psql('postgres', q{
	BEGIN;
	SELECT test_cs_put('pages', big(2002));
	ROLLBACK;
});
my $v = $primary->background_psql('postgres');
$v->query_safe(q{SELECT injection_points_set_local()});
$v->query_safe(q{SELECT injection_points_attach('chunk-index-vacuum-cut', 'wait')});
$v->query_until(qr/vacuuming/, "\\echo vacuuming\nVACUUM pg_history.chunk_index;\n");
$primary->wait_for_event('client backend', 'chunk-index-vacuum-cut');
# (the put, its entry in and found running by VACUUM, cancelled: aborted)
$primary->safe_psql('postgres',
	q{SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE application_name = 'entered'});
$primary->poll_query_until('postgres',
	q{SELECT count(*) = 0 FROM pg_stat_activity WHERE application_name = 'entered' AND state = 'active'})
  or die 'the put did not end';
like($p->query('SELECT 1') . $p->{stderr}, qr/canceling statement/, 'the put cancelled after its entry, before its bytes');
$p->quit;
my $before_cut = $primary->safe_psql('postgres', $size);
$primary->safe_psql('postgres', q{
	SELECT injection_points_detach('chunk-index-vacuum-cut');
	SELECT injection_points_wakeup('chunk-index-vacuum-cut');
});
$v->query_safe('SELECT 1');
$v->quit;
my $after_cut = $primary->safe_psql('postgres', $size);
cmp_ok($after_cut, '<', $before_cut, 'VACUUM cut the tail, the aborted put\'s pages with it');
# (puts after the cut, past a region of them, in pages made again where the
# aborted put's were)
$primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE (test_cs_put('pages', big(i))).stored) FROM generate_series(4001, 4040) i;
});
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
is($primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE test_cs_get('pages', h(big(i))) = big(i)) FROM generate_series(4001, 4040) i}),
	'40', 'the next VACUUM, taking the aborted entry out, counts nothing of its pages cut: those made again are the puts\' after');
is($primary->safe_psql('postgres', $audit), 't', 'the audit clean');

# 3. puts after the cut, freed, and the tail cut again; then the chunks put
# before both cuts freed (two generations old); everything freed, the log
# back to its meta page
$primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE (test_cs_put('pages', big(i))).stored) FROM generate_series(3001, 3020) i;
	SELECT test_cs_free('pages', ARRAY(SELECT h(big(i)) FROM generate_series(3001, 3020) i));
});
my $grown = $primary->safe_psql('postgres', $size);
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
cmp_ok($primary->safe_psql('postgres', $size), '<', $grown, 'a second cut');
$primary->safe_psql('postgres', q{SELECT test_cs_free_unheld('pages')});
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
is($primary->safe_psql('postgres', $size), $primary->safe_psql('postgres', q{SELECT current_setting('block_size')}),
	'everything freed, the log is its meta page: no byte left uncounted');

$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $size), $primary->safe_psql('postgres', $size),
	'the standby replayed it all, every page checked against the primary\'s');
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $audit), 't', 'and crash recovery');

$standby->stop;
$primary->stop;
done_testing();
