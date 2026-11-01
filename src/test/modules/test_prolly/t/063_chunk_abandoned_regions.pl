# Copyright (c) 2026, PostgreSQL Global Development Group

# A backend's region of a chunk log, the backend gone, is let go of by
# VACUUM: the log's map marks the pages handed to regions, and VACUUM makes
# each one marked full that no live backend's region has.
# - a crash: the regions of every backend, gone with it, let go of;
# - a backend gone during VACUUM, its put's entry put and its bytes not
#   written: that VACUUM leaves its region (the entry, aborted, is the next
#   one's, which counts its bytes dead, and then lets the region go);
# - a backend whose region begins after VACUUM's pass began, gone before
#   the pass ends, its put's entry put and bytes not written: that VACUUM
#   leaves its region too (the pass may not have seen the entry);
# - a page free taken by a backend as VACUUM's pass began (after it found
#   the regions held, before it read the page), its put's entry put after
#   the pass and bytes not written, the backend gone before the pass
#   ends: the page changed since the pass began, VACUUM leaves it;
# - a backend idle with a region over pages a tail cut took, others' after
#   the cut over them let go of all the same;
# - a log past its first map page's group: its bytes past the map page,
#   regions there let go of; a tail cut ending in the second group, and
#   regions after it, over pages marked before the cut; and every page
#   freed, the log cut back to its meta page, map pages and all.
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

# 3. a backend whose region begins after VACUUM's pass began, gone (the
# put's entry in a bucket the pass had passed) before the pass ends
$v = $primary->background_psql('postgres');
$v->query_safe(q{SELECT injection_points_set_local()});
$v->query_safe(q{SELECT injection_points_attach('chunk-index-vacuum-sweep', 'wait')});
$v->query_until(qr/vacuuming/, "\\echo vacuuming\nVACUUM pg_history.chunk_index;\n");
$primary->wait_for_event('client backend', 'chunk-index-vacuum-sweep');
$p = $primary->background_psql('postgres', on_error_stop => 0);
$p->query_safe(q{SELECT (test_cs_put('pages', blob(202))).stored});
$p->query_safe(q{SELECT injection_points_set_local()});
$p->query_safe(q{SELECT injection_points_attach('chunk-put-entered', 'wait')});
$p->query_safe(q{SET application_name = 'entered'});
$p->query_until(qr/putting/, "\\echo putting\nSELECT test_cs_put('pages', blob(201));\n");
$primary->wait_for_event('client backend', 'chunk-put-entered');
$primary->safe_psql('postgres',
	q{SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'entered'});
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
# (the next takes the entry out, and the one after lets the region go)
for my $i (1 .. 2)
{
	$stderr = '';
	$ok = $primary->psql('postgres', 'VACUUM pg_history.chunk_index', stderr => \$stderr);
	is($ok . $stderr, '0', "VACUUM $i after counts the gone put's bytes dead once");
}
is($primary->safe_psql('postgres', $audit), 't', 'the audit clean');
is($primary->safe_psql('postgres', q{SELECT test_cs_get('pages', h(blob(202))) = blob(202)}), 't',
	'the chunk put on the region\'s page reads back');

# 4. a page free taken as VACUUM's pass begins, after it found the regions
# held (its LSN, then, past the pass's beginning)
$primary->safe_psql('postgres', q{
	CREATE TABLE fl () USING prolly_chunks;
	CREATE TABLE fi () USING prolly_chunk_index;
	CREATE FUNCTION mid(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'L'::bytea || decode(repeat(md5('mid' || i), 120), 'hex')$$;
	SELECT count(*) FILTER (WHERE test_chunk_put('fl', 'fi', h(mid(i)), mid(i))) FROM generate_series(1, 20) i;
});
# (an anchor past them, so that their pages, freed, are not cut)
$primary->safe_psql('postgres', q{
	SELECT test_chunk_put('fl', 'fi', h(huge(1)), huge(1));
	SELECT roots FROM test_chunk_ref_root('fi', h(huge(1)));
	SELECT test_chunk_free_unheld('fl', 'fi');
});
$primary->safe_psql('postgres', 'VACUUM fi');
cmp_ok($primary->safe_psql('postgres', q{SELECT free_pages FROM test_chunk_log_space('fl')}), '>', 0,
	'pages free in the log\'s middle');
$v = $primary->background_psql('postgres');
$v->query_safe(q{SELECT injection_points_set_local()});
$v->query_safe(q{SELECT injection_points_attach('chunk-log-sweep-begin', 'wait')});
$v->query_safe(q{SELECT injection_points_attach('chunk-index-vacuum-sweep', 'wait')});
$v->query_until(qr/vacuuming/, "\\echo vacuuming\nVACUUM fi;\n");
$primary->wait_for_event('client backend', 'chunk-log-sweep-begin');
$p = $primary->background_psql('postgres', on_error_stop => 0);
$p->query_safe(q{SELECT test_chunk_put('fl', 'fi', h(blob(301)), blob(301))});
$primary->safe_psql('postgres', q{
	SELECT injection_points_detach('chunk-log-sweep-begin');
	SELECT injection_points_wakeup('chunk-log-sweep-begin');
});
$primary->wait_for_event('client backend', 'chunk-index-vacuum-sweep');
$p->query_safe(q{SELECT injection_points_set_local()});
$p->query_safe(q{SELECT injection_points_attach('chunk-put-entered', 'wait')});
$p->query_safe(q{SET application_name = 'entered'});
$p->query_until(qr/putting/, "\\echo putting\nSELECT test_chunk_put('fl', 'fi', h(blob(302)), blob(302));\n");
$primary->wait_for_event('client backend', 'chunk-put-entered');
$primary->safe_psql('postgres',
	q{SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'entered'});
$primary->poll_query_until('postgres', q{SELECT test_chunk_log_regions('fl') = 0})
  or die 'the put\'s backend did not exit';
$p->{run}->finish;
$primary->safe_psql('postgres', q{
	SELECT injection_points_detach('chunk-index-vacuum-sweep');
	SELECT injection_points_wakeup('chunk-index-vacuum-sweep');
});
$v->query_safe('SELECT 1');
$v->quit;
for my $i (1 .. 2)
{
	$stderr = '';
	$ok = $primary->psql('postgres', 'VACUUM fi', stderr => \$stderr);
	is($ok . $stderr, '0', "VACUUM $i after counts the gone put's bytes dead once");
}
is($primary->safe_psql('postgres', q{SELECT ok FROM test_chunk_check('fl', 'fi')}), 't', 'the audit clean');
is($primary->safe_psql('postgres', q{SELECT test_chunk_get('fl', 'fi', h(blob(301))) = blob(301)}), 't',
	'the chunk put on the page taken reads back');

# 5. a backend idle with a region over pages a tail cut takes: others'
# regions after the cut over the same pages, their backends gone, let go of
$primary->safe_psql('postgres', q{
	CREATE TABLE gl () USING prolly_chunks;
	CREATE TABLE gi () USING prolly_chunk_index;
	SELECT test_chunk_put('gl', 'gi', h(blob(400)), blob(400));
	SELECT roots FROM test_chunk_ref_root('gi', h(blob(400)));
});
my $idle = $primary->background_psql('postgres');
$idle->query_safe(q{SELECT test_chunk_put('gl', 'gi', h(huge(400)), huge(400))});
$primary->safe_psql('postgres', q{SELECT test_chunk_free_unheld('gl', 'gi')});
my $gsize = q{SELECT pg_relation_size('gl') / current_setting('block_size')::int};
my $grown = $primary->safe_psql('postgres', $gsize);
$primary->safe_psql('postgres', 'VACUUM gi');
cmp_ok($primary->safe_psql('postgres', $gsize), '<', $grown, 'the idle backend\'s pages cut');
is($primary->safe_psql('postgres', q{SELECT test_chunk_log_regions('gl')}), '0',
	'its region, of the generation before the cut, holds nothing');
# (a chunk longer than a region, from the cut on: its last page, part
# written, in the pages the idle backend's region had)
$primary->safe_psql('postgres', q{SELECT test_chunk_put('gl', 'gi', h(huge(401)), huge(401))});
$primary->safe_psql('postgres', 'VACUUM gi');
is($primary->safe_psql('postgres', q{SELECT pages = full_pages FROM test_chunk_log_space('gl')}), 't',
	'a region after the cut over its pages, its backend gone, let go of');
is($primary->safe_psql('postgres', q{SELECT ok FROM test_chunk_check('gl', 'gi')}), 't', 'the audit clean');
$idle->quit;
# (a region forgotten to another's invalidation, taken in as a transaction
# begins, no put running: let go of at once, not as that transaction ends)
my $a = $primary->background_psql('postgres', on_error_stop => 0);
$a->query_safe(q{SELECT test_chunk_put('gl', 'gi', h(blob(420)), blob(420))});
is($primary->safe_psql('postgres', q{SELECT test_chunk_log_regions('gl')}), '1', 'a backend\'s region');
$primary->safe_psql('postgres', 'GRANT SELECT ON gl TO PUBLIC');
$a->query_safe('BEGIN');
$a->query_safe('SELECT 1');
is($primary->safe_psql('postgres', q{SELECT test_chunk_log_regions('gl')}), '0',
	'forgotten to another\'s invalidation, let go of at once');
$a->query_safe('COMMIT');
# (and so after a put that failed in a subtransaction, rolled back to)
$a->query_safe(q{BEGIN});
$a->query_safe(q{SELECT test_chunk_put('gl', 'gi', h(blob(421)), blob(421))});
$a->query_safe(q{SAVEPOINT s});
$a->query_safe(q{SELECT injection_points_set_local()});
$a->query_safe(q{SELECT injection_points_attach('chunk-log-region-sealed', 'error')});
$a->query(q{SELECT test_chunk_put('gl', 'gi', h(huge(421)), huge(421))});
like($a->{stderr}, qr/error triggered for injection point chunk-log-region-sealed/, 'a put failing');
$a->{stderr} = '';
$a->query_safe(q{ROLLBACK TO s});
$a->query_safe(q{SELECT injection_points_detach('chunk-log-region-sealed')});
is($primary->safe_psql('postgres', q{SELECT test_chunk_log_regions('gl')}), '1', 'its region after');
$primary->safe_psql('postgres', 'REVOKE SELECT ON gl FROM PUBLIC');
$a->query_safe(q{SELECT test_chunk_log_regions('gl')});
is($primary->safe_psql('postgres', q{SELECT test_chunk_log_regions('gl')}), '0',
	'forgotten after a put failed in a subtransaction, let go of at once');
$a->query_safe('COMMIT');
$a->quit;

# 6. a log past its first map page's group (4096 data pages)
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
# (a tail cut ending in the second group, after huge(105)'s pages; puts
# after it over pages marked before the cut, by a backend that exits)
my $blocks = q{SELECT pg_relation_size('ml') / current_setting('block_size')::int};
$primary->safe_psql('postgres', q{
	SELECT roots FROM test_chunk_ref_root('mi', h(huge(105)));
	SELECT roots FROM test_chunk_unref_root('mi', h(huge(110)));
	SELECT test_chunk_free_unheld('ml', 'mi');
});
my $before_cut = $primary->safe_psql('postgres', $blocks);
$primary->safe_psql('postgres', 'VACUUM mi');
my $after_cut = $primary->safe_psql('postgres', $blocks);
cmp_ok($after_cut, '<', $before_cut, 'a tail cut');
cmp_ok($after_cut, '>', 4098, '... ending in the second group');
$primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE test_chunk_put('ml', 'mi', h(huge(i)), huge(i))) FROM generate_series(201, 203) i;
	SELECT test_chunk_put('ml', 'mi', h(blob(8)), blob(8));
});
$primary->safe_psql('postgres', 'VACUUM mi');
is($primary->safe_psql('postgres', q{SELECT pages = full_pages FROM test_chunk_log_space('ml')}), 't',
	'regions after the cut, their backend gone, let go of');
is($primary->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE test_chunk_get('ml', 'mi', h(huge(i))) = huge(i)) FROM generate_series(201, 203) i}),
	'3', 'their chunks read back');
is($primary->safe_psql('postgres', q{SELECT ok FROM test_chunk_check('ml', 'mi')}), 't', 'the audit clean');
$primary->safe_psql('postgres', q{
	SELECT roots FROM test_chunk_unref_root('mi', h(huge(105)));
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
