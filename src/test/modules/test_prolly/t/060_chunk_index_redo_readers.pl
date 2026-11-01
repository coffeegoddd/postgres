# Copyright (c) 2026, PostgreSQL Global Development Group

# A hot standby's readers of the chunk index see each of its records whole,
# as readers on the primary do: replay holds each page a record changes
# locked, the bucket's first page among them, until the record is made.
# Replay is stopped in a split's record, the meta page's masks made but not
# the directory's slot, and in a new page's, the chain's last page pointing
# at it but it not made: a reader then waits, and finds every chunk.  And
# in a fold of a change into its chunk's entry, both on pages of the
# chain past the bucket's first, the change's page locked but not the
# chunk's: a reader (its cache of the index's masks warm, so going to the
# bucket at once) waits at the bucket's first page, and sees the chunk's
# counts whole (one walking the chain meanwhile would find the chunk's
# entry before the fold and its change gone after).

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

# a fold on pages past a bucket's first: in an index of its own (eight
# buckets), a chunk D and many changes of its counts filling the bucket's
# first page, then a chunk C there (its entry on the second page), more of
# D's changes filling that, then a change of C's (on a page after)
$primary->safe_psql('postgres', q{
	CREATE TABLE fl () USING prolly_chunks;
	CREATE TABLE fi () USING prolly_chunk_index;
	CREATE TABLE fb AS
		SELECT i, int4send(i) AS d FROM generate_series(1, 100000) i
		WHERE get_byte(sha256(int4send(i)), 3) & 7 = 5 LIMIT 2;
	SELECT test_chunk_put('fl', 'fi', substr(sha256(d), 1, 20), d) FROM fb WHERE i = (SELECT min(i) FROM fb);
});
my $hd = q{(SELECT substr(sha256(d), 1, 20) FROM fb WHERE i = (SELECT min(i) FROM fb))};
my $hc = q{(SELECT substr(sha256(d), 1, 20) FROM fb WHERE i = (SELECT max(i) FROM fb))};
my $changes = sub {
	my ($n) = @_;
	$primary->safe_psql('postgres', qq{
		DO \$\$BEGIN
			FOR k IN 1 .. $n LOOP
				PERFORM test_chunk_index_adjust('fi', $hd, 0, 1);
				COMMIT;
			END LOOP;
		END\$\$;
	});
};
$changes->(200);
$primary->safe_psql('postgres', q{
	SELECT test_chunk_put('fl', 'fi', substr(sha256(d), 1, 20), d) FROM fb WHERE i = (SELECT max(i) FROM fb);
});
$changes->(300);
$primary->safe_psql('postgres', qq{SELECT test_chunk_index_adjust('fi', $hc, 0, 1)});
$primary->wait_for_replay_catchup($standby);
my $c_counts = qq{SELECT refs || ' ' || roots FROM test_chunk_index_entry('fi', $hc)};
is($standby->safe_psql('postgres', $c_counts), '0 1', 'the standby has C\'s change');

$standby->safe_psql('postgres', "SELECT injection_points_attach('chunk-index-redo-fold', 'wait')");
my $reader = $standby->background_psql('postgres', on_error_stop => 0);
# (its cache warm: the index's masks read)
is($reader->query_safe($c_counts), '0 1', 'a reader, its cache of the index warm');
$primary->safe_psql('postgres', 'VACUUM fi');
$standby->wait_for_event('startup', 'chunk-index-redo-fold');
$reader->query_until(qr/reading/, "\\echo reading\n$c_counts;\n");
$standby->safe_psql('postgres', q{
	SELECT injection_points_detach('chunk-index-redo-fold');
	SELECT injection_points_wakeup('chunk-index-redo-fold');
});
is($reader->query('SELECT 1'), "0 1\n1", 'stopped in a fold past the bucket\'s first page, a reader sees the chunk\'s counts whole');
is($reader->{stderr}, '', 'and no error');
$reader->quit;
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $c_counts), '0 1', 'and after');

$standby->stop;
$primary->stop;
done_testing();
