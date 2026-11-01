# Copyright (c) 2026, PostgreSQL Global Development Group

# Many backends put chunks into the history store at once, through its
# index: buckets split under them, the same chunk is put by several at
# once, and every chunk put is found with its bytes, during the run, after
# a crash and on a standby.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('chunk_index');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE cl () USING prolly_chunks;
	CREATE TABLE ci () USING prolly_chunk_index;
	CREATE TABLE put (hash bytea, digest text);
});
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('chunk_index_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

# each transaction puts a chunk of one of 20,000 contents: most are put by
# more than one client, some at once
$node->pgbench(
	'--no-vacuum --client=8 --jobs=4 --transactions=4000',
	0,
	[qr{processed: 32000/32000}],
	[qr{^$}],
	'eight clients put chunks at once',
	{
		'051_put' => q{
\set n random(1, 20000)
BEGIN;
INSERT INTO put SELECT substr(sha256(d), 1, 20), md5(d)
  FROM (SELECT convert_to(repeat('chunk :n ', :n % 50 + 1), 'UTF8') AS d) s
  WHERE test_chunk_put('cl', 'ci', substr(sha256(d), 1, 20), d) IS NOT NULL;
COMMIT;
}
	});

my $check = q{SELECT count(*), count(*) FILTER (WHERE md5(test_chunk_get('cl', 'ci', hash)) = digest)
	FROM (SELECT DISTINCT hash, digest FROM put) s};
my ($n) = split /\|/, $node->safe_psql('postgres', $check);
ok($n > 10000, 'many distinct chunks were put');
is($node->safe_psql('postgres', $check), "$n|$n", 'every chunk put is found with its bytes');
like($node->safe_psql('postgres', q{SELECT buckets FROM test_chunk_index_shape('ci')}),
	qr/^\d{3,}$/, 'the index grew under them');

$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $check), "$n|$n", 'and after a crash');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $check), "$n|$n", 'and on a standby');

# a split an error stops after making the new bucket, before taking its
# entries out of the old one, as a crash there would: the old bucket keeps
# copies of them, which lookups pass by and inserts take out
SKIP:
{
	skip 'injection points not supported by this build', 3
	  if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', q{
		CREATE EXTENSION injection_points;
		CREATE TABLE ci2 () USING prolly_chunk_index;
		CREATE TABLE put2 (hash bytea, digest text);
		SELECT injection_points_attach('chunk-index-split-published', 'error');
		DO $$
		DECLARE
			d bytea;
		BEGIN
			FOR i IN 1 .. 20000 LOOP
				d := convert_to('split ' || i, 'UTF8');
				BEGIN
					INSERT INTO put2 VALUES (substr(sha256(d), 1, 20), md5(d));
					PERFORM test_chunk_put('cl', 'ci2', substr(sha256(d), 1, 20), d);
				EXCEPTION WHEN others THEN
					EXIT;
				END;
			END LOOP;
		END $$;
		SELECT injection_points_detach('chunk-index-split-published');
	});
	my $check2 = q{SELECT count(*), count(*) FILTER (WHERE md5(test_chunk_get('cl', 'ci2', hash)) = digest) FROM put2};
	my ($n2) = split /\|/, $node->safe_psql('postgres', $check2);
	is($node->safe_psql('postgres', $check2), "$n2|$n2",
		'every chunk is found after a split stopped halfway');
	# more puts go through the old bucket; then every chunk removed is gone,
	# with no copy left to find
	$node->safe_psql('postgres', q{
		SELECT count(test_chunk_put('cl', 'ci2', substr(sha256(d), 1, 20), d))
		FROM (SELECT convert_to('more ' || g, 'UTF8') AS d FROM generate_series(1, 2000) g) s;
	});
	is($node->safe_psql('postgres', q{SELECT count(*) FILTER (WHERE test_chunk_index_remove('ci2', hash)) FROM put2}),
		$n2, 'each removed once');
	is($node->safe_psql('postgres', q{SELECT count(*) FILTER (WHERE test_chunk_has('cl', 'ci2', hash)) FROM put2}),
		'0', 'and none found again');
}

$standby->stop;
$node->stop;
done_testing();
