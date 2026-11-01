# Copyright (c) 2026, PostgreSQL Global Development Group

# Many backends append to the history store's chunk log at once, each into
# a region of its own: every append's bytes read back as written, during
# the run and after a crash, and on a standby.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('chunk_writers');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE chunks () USING prolly_chunks;
	CREATE TABLE placed (loc int8, len int4, digest text);
});
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('chunk_writers_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

# each transaction appends a chunk of 32 to 20,032 bytes and notes where it
# went (most span pages)
$node->pgbench(
	'--no-vacuum --client=8 --jobs=4 --transactions=300',
	0,
	[qr{processed: 2400/2400}],
	[qr{^$}],
	'eight clients append at once',
	{
		'050_append' => q{
\set n random(1, 20000)
BEGIN;
INSERT INTO placed SELECT test_chunk_log_append('chunks', d), length(d), md5(d)
  FROM (SELECT convert_to(repeat(md5(random()::text), :n / 32 + 1), 'UTF8') AS d) s;
COMMIT;
}
	});

my $check = q{SELECT count(*), count(*) FILTER (WHERE md5(test_chunk_log_read('chunks', loc, len)) = digest),
	count(DISTINCT loc) FROM placed};
is($node->safe_psql('postgres', $check), '2400|2400|2400', 'every append reads back as written');

# no two appends overlap
is($node->safe_psql('postgres', q{SELECT count(*) FROM (
	SELECT loc, len, lead(loc) OVER (ORDER BY loc) AS next FROM placed) s
	WHERE loc + len > next}), '0', 'no two appends overlap');

$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $check), '2400|2400|2400', 'and after a crash');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $check), '2400|2400|2400', 'and on a standby');

# an append an error stops partway leaves this backend's region where its
# last page ends: the session's next appends go on after it
SKIP:
{
	skip 'injection points not supported by this build', 2
	  if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	my ($ret, $out, $err) = $node->psql('postgres', q{
		SELECT injection_points_attach('chunk-log-append-page', 'error');
		SELECT test_chunk_log_append('chunks', convert_to(repeat('x', 20000), 'UTF8'));
		SELECT injection_points_detach('chunk-log-append-page');
		SELECT test_chunk_log_read('chunks', test_chunk_log_append('chunks', '\x0102'), 2) = '\x0102',
			md5(test_chunk_log_read('chunks',
				test_chunk_log_append('chunks', convert_to(repeat('y', 20000), 'UTF8')), 20000))
			= md5(convert_to(repeat('y', 20000), 'UTF8'));
	}, on_error_stop => 0);
	like($err, qr/error triggered for injection point chunk-log-append-page/,
		'an append stopped after its first page');
	like($out, qr/^t\|t$/m, 'the same session appends after it');
}

$standby->stop;
$node->stop;
done_testing();
