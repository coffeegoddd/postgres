# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum's automatic collection, refused because another collection of
# the store is running, skips quietly: a DEBUG1 message and
# pg_stat_prolly's auto_collections_skipped, not an ERROR in the log.  An
# explicit dolt_gc() refused so still reports the error, and any other
# error an automatic collection meets is still reported.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('autovac_gc_busy');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
prolly.gc_journal_threshold = 1MB
prolly.gc_growth_scale_factor = 0
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE av');
$node->safe_psql('av', 'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');

sub sql { return $node->safe_psql('av', $_[0]); }

my $skipped = q{SELECT value FROM pg_stat_prolly WHERE name = 'auto_collections_skipped'};

# A collection that holds the store's collection lock, stopped after its
# swap (the lock is held to its transaction's end, the manifest's isn't).
my $g = $node->background_psql('av');
$g->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-swapped', 'wait')});
$g->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo done
});
$node->poll_query_until('av', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-gc-swapped'}) or die 'the collection did not stop';

# Past the threshold while it runs: autovacuum's collections are skipped.
my $log_offset = -s $node->logfile;
sql(q{SELECT count(test_chunk_put(test_noise(g::text, 100000)))
	FROM generate_series(1, 20) g});
ok($node->poll_query_until('av', "SELECT ($skipped) > 0"),
	'an automatic collection while another runs is skipped and counted');

# An explicit one is still refused with the error.
my ($ret, $out, $err) = $node->psql('av', 'SELECT dolt_gc()', timeout => 30);
like($err, qr/another collection of this database's chunk store is running/,
	'an explicit collection while another runs is refused');

sql(q{SELECT injection_points_wakeup('prolly-gc-swapped')});
$g->query_until(qr/done/, '');
$g->quit;

# The autovacuum workers logged no refusal.
my $log = slurp_file($node->logfile, $log_offset);
my @refusals = grep { /ERROR:  another collection/ } split /\n/, $log;
is(scalar(@refusals), 1, "the log's only refusal is the explicit collection's")
	or diag(join("\n", @refusals));

# Any other error an automatic collection meets is still reported, and
# not counted as a skip.
my $count = sql($skipped);
$log_offset = -s $node->logfile;
sql(q{SELECT injection_points_attach('prolly-gc-roots-scanned', 'error')});
sql(q{SELECT count(test_chunk_put(test_noise(g::text, 100002)))
	FROM generate_series(1, 20) g});
ok( $node->wait_for_log(
		qr/autovacuum worker.*ERROR:  error triggered for injection point prolly-gc-roots-scanned/,
		$log_offset),
	'another error of an automatic collection is reported');
sql(q{SELECT injection_points_detach('prolly-gc-roots-scanned')});
is(sql($skipped), $count, 'and not counted as skipped');

# With the store free again, the next automatic collection runs.
my $dead = sql(q{SELECT test_chunk_put('dead after the skip')});
sql(q{SELECT count(test_chunk_put(test_noise(g::text, 100001)))
	FROM generate_series(1, 20) g});
ok($node->poll_query_until('av', qq{SELECT NOT test_chunk_has('$dead')}),
	'once the other collection ends, autovacuum collects');

$node->stop;
done_testing();
