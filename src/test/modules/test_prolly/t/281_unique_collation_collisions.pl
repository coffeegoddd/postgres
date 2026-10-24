# Copyright (c) 2026, PostgreSQL Global Development Group

# A merge records each row a refused row collides with in a unique index
# by the index's own equality: under a case-insensitive collation, the
# 'Hello' row a refused 'hello' collides with is recorded too.  (Needs a
# build with ICU, for a nondeterministic collation.)

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('collisions');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off");
$node->start;

if ($node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_collation WHERE collprovider = 'i'}) == 0)
{
	$node->stop;
	plan skip_all => 'ICU not supported by this build';
}

$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2',
		deterministic = false);
	CREATE SCHEMA c VERSIONED;
	CREATE TABLE c.t (pk int PRIMARY KEY, s text);
	CREATE UNIQUE INDEX t_s_ci ON c.t (s COLLATE ci);
	INSERT INTO c.t VALUES (1, 'base');
	SELECT length(dolt_commit('-Am', 'base')::text);
	SELECT dolt_branch('side');
	INSERT INTO c.t VALUES (10, 'Hello');
	SELECT length(dolt_commit('-am', 'ours')::text);
	SELECT dolt_checkout('side');
	INSERT INTO c.t VALUES (20, 'hello');
	SELECT length(dolt_commit('-am', 'theirs')::text);
	SELECT dolt_checkout('main');
});
is( $node->safe_psql('postgres', q{
		BEGIN;
		SELECT FROM dolt_merge('side');
		SELECT string_agg((violating_row->>'pk') || ':' || (violation_info->>'Name'),
			',' ORDER BY violating_row->>'pk')
		FROM dolt_constraint_violations('c.t');
		ROLLBACK;
	}),
	'10:t_s_ci,20:t_s_ci',
	'the row collided with under the index collation is recorded');

$node->stop;
done_testing();
