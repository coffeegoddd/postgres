# Copyright (c) 2026, PostgreSQL Global Development Group

# The queries a merge completes unique violations with: each finds the
# rows a batch of refused rows collides with through the unique index
# (the table isn't scanned whole), and a batch is bounded by its rows'
# bytes, so refused rows with large values make several small queries,
# not one too large to build.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('collision_queries');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off");
$node->start;

# The completion queries a merge of branch logs (at DEBUG1), in a
# transaction rolled back.
sub completion_queries
{
	my ($branch) = @_;
	my $offset = -s $node->logfile;
	$node->safe_psql('postgres', qq{
		SET log_min_messages = debug1;
		BEGIN;
		SELECT FROM dolt_merge('$branch');
		ROLLBACK;
	});
	my $log = substr(slurp_file($node->logfile), $offset);
	my @q;
	while ($log =~ /prolly merge unique completion query: (.*)/g)
	{
		push @q, $1;
	}
	return @q;
}

$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA c VERSIONED;
	CREATE TABLE c.t (pk int PRIMARY KEY, a int UNIQUE, pad text);
	INSERT INTO c.t SELECT g, g, 'p' FROM generate_series(1, 20000) g;
	SELECT length(dolt_commit('-Am', 'base')::text);
	SELECT dolt_branch('side');
	SELECT dolt_branch('big');
	INSERT INTO c.t SELECT 100000 + g, 500000 + g, 'ours' FROM generate_series(1, 20) g;
	SELECT length(dolt_commit('-am', 'ours')::text);
	SELECT dolt_checkout('side');
	INSERT INTO c.t SELECT 200000 + g, 500000 + g, 'theirs' FROM generate_series(1, 10) g;
	SELECT length(dolt_commit('-am', 'theirs')::text);
	SELECT dolt_checkout('big');
	INSERT INTO c.t SELECT 300000 + g, 500000 + g, repeat(md5(g::text), 10000)
	  FROM generate_series(1, 20) g;
	SELECT length(dolt_commit('-am', 'big')::text);
	SELECT dolt_checkout('main');
	ANALYZE c.t;
});

my @q = completion_queries('side');
is(scalar(@q), 1, 'one query, for the one unique index but the key');
my $plan = $node->safe_psql('postgres', "EXPLAIN (COSTS OFF) $q[0]");
like($plan, qr/Index (Only )?Scan using t_a_key/, 'it looks the rows up by the index')
  or diag($plan);
unlike($plan, qr/Seq Scan on t\b/, 'and scans no table whole');

# 20 refused rows of 320kB each: batches of no more than about 1MB of rows
@q = completion_queries('big');
my $longest = 0;
for (@q) { $longest = length($_) if length($_) > $longest; }
cmp_ok(scalar(@q), '>=', 4, 'large refused rows make several batches');
cmp_ok($longest, '<', 4 * 1024 * 1024, 'each query is a few MB at most');
is($node->safe_psql('postgres', q{
		BEGIN;
		SELECT FROM dolt_merge('big');
		SELECT count(*) FROM dolt_constraint_violations('c.t');
		ROLLBACK;
	}), '40', 'each refused row and the row it collides with are recorded');

$node->stop;
done_testing();
