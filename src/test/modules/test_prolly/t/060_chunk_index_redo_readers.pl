# Copyright (c) 2026, PostgreSQL Global Development Group

# A hot standby's readers of the chunk index see each of its records whole,
# as readers on the primary do: replay holds each page a record changes
# locked, the bucket's first page among them, until the record is made.
# Replay is stopped in a split's record, the meta page's masks made but not
# the directory's slot, and in a new page's, the chain's last page pointing
# at it but it not made: a reader then waits, and finds every chunk.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('redo_readers_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_free_in_background = off
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE FUNCTION h(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(int4send(i)), 1, 20)';
	CREATE FUNCTION put(i int) RETURNS bool LANGUAGE sql
		AS $$SELECT test_chunk_put('pg_history.chunks', 'pg_history.chunk_index', h(i), int4send(i))$$;
	SELECT count(*) FILTER (WHERE put(i)) FROM generate_series(1, 3000) i;
});
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('redo_readers_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;
$primary->wait_for_replay_catchup($standby);

my $all = q{SELECT count(*) FILTER (WHERE test_chunk_has('pg_history.chunks',
	'pg_history.chunk_index', h(i))) FROM generate_series(1, 3000) i};
is($standby->safe_psql('postgres', $all), '3000', 'the standby has every chunk');

my $next = 3001;
for my $point ('chunk-index-redo-split', 'chunk-index-redo-add-page')
{
	$standby->safe_psql('postgres', "SELECT injection_points_attach('$point', 'wait')");

	# puts on the primary until replay stops in the record
	my $from = $next;
	$next += 1000;
	$primary->safe_psql('postgres',
		"SELECT count(*) FILTER (WHERE put(i)) FROM generate_series($from, $next - 1) i");
	$standby->wait_for_event('startup', $point);

	# a reader meanwhile: waiting, then finding every chunk (seeing the
	# record in part, it would read a page not yet a bucket's)
	my $reader = $standby->background_psql('postgres', on_error_stop => 0);
	$reader->query_until(qr/reading/, "\\echo reading\n$all;\n");
	$standby->safe_psql('postgres', qq{
		SELECT injection_points_detach('$point');
		SELECT injection_points_wakeup('$point');
	});
	my $out = $reader->query('SELECT 1');
	is($out, "3000\n1", "stopped in $point, a reader finds every chunk");
	is($reader->{stderr}, '', 'and no error');
	$reader->quit;
}

$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $all), '3000', 'every chunk there after');

$standby->stop;
$primary->stop;
done_testing();
