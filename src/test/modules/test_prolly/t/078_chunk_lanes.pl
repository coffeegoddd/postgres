# Copyright (c) 2026, PostgreSQL Global Development Group

# Many backends writing the history store at once never wait on each
# other to grow it: each appends into a lane of its own (a relation that
# only it extends), so no relation's extension lock is ever taken by two.
# Their chunks read back, from any backend; the lanes are the log's, and
# go with it.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('lanes');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
max_connections = 100
log_lock_waits = on
deadlock_timeout = 10ms
log_line_prefix = '%m [%p] %b %a: '
log_min_error_statement = log
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE SEQUENCE n;
	-- (a chunk's bytes from its number: nothing kept of the puts, so the
	-- test itself grows no relation the writers share)
	CREATE FUNCTION d(i int8) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(repeat('lane chunk ' || i || ' ', 300), 'UTF8')$$;
	CREATE FUNCTION h(b bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(b), 1, 20)';
});

# sixty-four backends, each putting chunks of a few kB (pages of their own
# lane filling, and the lane growing, all the while)
my $log_at = -s $node->logfile;
$node->pgbench(
	'--no-vacuum --client=64 --jobs=8 --transactions=40',
	0,
	[qr{processed: 2560/2560}],
	[qr{^$}],
	'sixty-four backends write the history store at once',
	{
		'078_put' => q{
SELECT (test_cs_put('pages', d(nextval('n')))).stored;
}
	});

# (a wait's lines, its relation named, shown if there is one)
my $waits = slurp_file($node->logfile, $log_at);
my %rels = map { $_ => 1 } ($waits =~ /of relation (\d+) of database/g);
my @named = map {
	"relation $_ is "
	  . $node->safe_psql('postgres',
		"SELECT coalesce((SELECT oid::regclass::text FROM pg_class WHERE oid = $_), 'gone')")
} sort keys %rels;
my @waited = grep { /still waiting/ .. /STATEMENT/ } split /\n/, $waits;
is(scalar(grep { /still waiting/ } @waited), 0, 'no backend waited on another')
  or diag(join("\n", @named, @waited));

# every chunk reads back, here, in one backend, from every lane
my $read_back = q{SELECT count(*) FILTER (WHERE test_cs_get('pages', h(d(i))) IS DISTINCT FROM d(i))
	FROM generate_series(1, 2560) i};
is($node->safe_psql('postgres', $read_back), '0', 'every chunk reads back');

# the lanes: relations of the log's own, each internally dependent on it
my $lanes = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_class WHERE relname LIKE 'chunks_lane_%'
		AND relnamespace = 'pg_history'::regnamespace});
cmp_ok($lanes, '>=', 2, "the writers had lanes of their own ($lanes beside the log's)");
is( $node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_class c JOIN pg_depend d ON d.objid = c.oid
		  WHERE c.relname LIKE 'chunks_lane_%' AND d.deptype = 'i'
			AND d.refobjid = 'pg_history.chunks'::regclass}),
	$lanes, 'each lane depends on the log');

# after a restart (crash recovery replaying every lane's WAL) the same
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $read_back), '0', 'every chunk reads back after crash recovery');
is( $node->safe_psql('postgres', q{SELECT ok FROM test_cs_check('pages')}),
	't', "the store's audit passes");

$node->stop;
done_testing();
