
# Copyright (c) 2026, PostgreSQL Global Development Group

# The history store's tier repacked in the background: packs more than the
# fraction dead, their dead bytes half a pack's or more, are compacted by
# the chunk freer VACUUM starts, their live chunks moved a pack's worth a
# transaction (into one pack, here: the pages' batch of chunks would take
# two), and the old packs, all dead then, removed; more live than a pack
# makes two packs, none of one chunk; and a pack with a chunk freed that a
# snapshot keeps is passed by.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tier = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('repack');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_compact_dead_fraction = 0.4
log_min_messages = debug1
history_tier_location = '$tier'
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION blob(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(rpad('rp' || i, 50000, '.'), 'UTF8')$$;
});
my $packs = q{SELECT string_agg(id::text, ',' ORDER BY id) FROM test_tier_packs('pg_history.tier_chunks') WHERE state <> 'removed' AND size > 0};

# some 75 MB in one transaction: two packs, each chunk a root
$node->safe_psql('postgres', q{
	BEGIN;
	SELECT count(*) FROM generate_series(1, 1500) g, LATERAL test_cs_put('tier', blob(g));
	SELECT count(*) FROM generate_series(1, 1500) g, LATERAL test_cs_ref_root('tier', h(blob(g)));
	COMMIT;
});
my $before = $node->safe_psql('postgres', $packs);
is(scalar(split(/,/, $before)), 2, 'two packs');

# two chunks of every three let go of and freed: each pack two thirds dead
$node->safe_psql('postgres', q{
	BEGIN;
	SELECT count(*) FROM generate_series(1, 1500) g, LATERAL test_cs_unref_root('tier', h(blob(g))) WHERE g % 3 <> 0;
	SELECT test_cs_free('tier', ARRAY(SELECT h(blob(g)) FROM generate_series(1, 1500) g WHERE g % 3 <> 0));
	COMMIT;
});

# VACUUM counts them dead, finds the packs to repack, and starts a freer
my $offset = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
$node->wait_for_log(qr/a freer to start: \d+ chunks unheld, 2 segments to compact/, $offset);
$node->wait_for_log(qr/chunk freer freed \d+ chunks of chunk index \d+, and moved 500 \(/, $offset);

# the next VACUUM takes out the entries moved: the old packs all dead, removed
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
my $after = $node->safe_psql('postgres', $packs);
my %old = map { $_ => 1 } split(/,/, $before);
my @new = grep { !$old{$_} } split(/,/, $after);
is(scalar(grep { $old{$_} } split(/,/, $after)), 0, 'the old packs removed');
is(scalar(@new), 1, 'the live chunks in one new pack');
is($node->safe_psql('postgres', qq{SELECT count(*) FROM test_tier_pack_table('pg_history.tier_chunks', $new[0])}),
	'500', 'its table of the 500 live chunks');
is($node->safe_psql('postgres', q{
	SELECT count(*) FILTER (WHERE test_cs_get('tier', h(blob(g))) = blob(g)) FROM generate_series(1, 1500) g WHERE g % 3 = 0}),
	'500', 'every one read back');
is($node->safe_psql('postgres', q{SELECT ok FROM test_cs_check('tier')}), 't', 'the audit clean');

# more live than a pack holds: two packs, a pack's worth a transaction,
# and no pack of one chunk (a step stops before the chunk that would pass
# the pack's bytes)
$node->safe_psql('postgres', q{
	BEGIN;
	SELECT count(*) FROM generate_series(10001, 13000) g, LATERAL test_cs_put('tier', blob(g));
	SELECT count(*) FROM generate_series(10001, 13000) g, LATERAL test_cs_ref_root('tier', h(blob(g)));
	COMMIT;
});
my $before2 = $node->safe_psql('postgres', $packs);
$node->safe_psql('postgres', q{
	BEGIN;
	SELECT count(*) FROM generate_series(10001, 13000) g, LATERAL test_cs_unref_root('tier', h(blob(g))) WHERE g % 2 = 0;
	SELECT test_cs_free('tier', ARRAY(SELECT h(blob(g)) FROM generate_series(10001, 13000) g WHERE g % 2 = 0));
	COMMIT;
});
$offset = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
$node->wait_for_log(qr/chunk freer freed \d+ chunks of chunk index \d+, and moved 1500 \(/, $offset);
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
my %old2 = map { $_ => 1 } split(/,/, $before2);
my @new2 = grep { !$old2{$_} } split(/,/, $node->safe_psql('postgres', $packs));
is(scalar(@new2), 2, 'more live than a pack: two new packs');
is($node->safe_psql('postgres', 'SELECT min(n) FROM (SELECT count(*) AS n FROM unnest(ARRAY[' . join(',', @new2) . "]) p, LATERAL test_tier_pack_table('pg_history.tier_chunks', p) t GROUP BY p) x") > 1,
	1, 'none of one chunk');

# a pack with a chunk freed that a snapshot keeps: passed by, counted apart
$node->safe_psql('postgres', 'ALTER SYSTEM SET chunk_free_in_background = off');
$node->reload;
$node->poll_query_until('postgres', q{SELECT current_setting('chunk_free_in_background') = 'off'})
  or die 'the freer setting not reloaded';
$node->safe_psql('postgres', q{
	BEGIN;
	SELECT count(*) FROM generate_series(20001, 20010) g, LATERAL test_cs_put('tier', blob(g));
	SELECT count(*) FROM generate_series(20001, 20010) g, LATERAL test_cs_ref_root('tier', h(blob(g)));
	COMMIT;
	SELECT count(*) FROM generate_series(20001, 20006) g, LATERAL test_cs_unref_root('tier', h(blob(g)));
	SELECT test_cs_free('tier', ARRAY(SELECT h(blob(g)) FROM generate_series(20001, 20006) g));
	VACUUM pg_history.tier_index;
});
my $snap = $node->background_psql('postgres');
$snap->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$snap->query_safe(q{SELECT length(test_cs_get('tier', h(blob(20007))))});
$node->safe_psql('postgres', q{
	SELECT test_cs_unref_root('tier', h(blob(20007)));
	SELECT test_cs_free('tier', ARRAY[h(blob(20007))]);
	SELECT (test_cs_put('tier', blob(29999))).stored;
});
$offset = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
ok($node->log_contains(qr/a freer to start: [1-9]\d* chunks unheld, \d+ segments to compact.*, [1-9]\d* passed by for snapshots/, $offset),
	'its pack passed by for the snapshot');
$snap->quit;

$node->stop;
done_testing();
