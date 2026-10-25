# Copyright (c) 2026, PostgreSQL Global Development Group

# The shared commit cache across backends, databases and a standby: what
# one backend read another finds; dropping a database forgets its
# entries; a standby's sessions fill their own server's cache, and a
# collection replayed there lets go of them, so a collected commit no
# longer resolves.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('commit_cache');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE cc');
$node->safe_psql('cc', q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v int);
	INSERT INTO s.t VALUES (1, 0);
	SELECT dolt_commit('-Am', 'c0');
	UPDATE s.t SET v = 1;
	SELECT dolt_commit('-am', 'c1');
	UPDATE s.t SET v = 2;
	SELECT dolt_commit('-am', 'c2');
});
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('commit_cache_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

# What resolving ref reads and finds in the cache, in a session of its own.
sub walk
{
	my ($n, $ref) = @_;
	# (counts before kept in a psql variable: a standby makes no table)
	return $n->safe_psql('cc', qq{
		SELECT json_object_agg(name, value) AS before FROM pg_stat_prolly \\gset
		SELECT dolt_hashof('$ref') IS NOT NULL;
		SELECT pg_stat_force_next_flush();
		SELECT string_agg(name || '=' || (value - (:'before'::json ->> name)::int8),
			' ' ORDER BY name)
		FROM pg_stat_prolly
		WHERE name IN ('commits_read', 'commit_cache_hits', 'commit_cache_misses');
	}) =~ s/.*\n//sr;
}

is(walk($node, 'HEAD~2'),
	'commit_cache_hits=0 commit_cache_misses=2 commits_read=2',
	'a first walk reads its commits');
is(walk($node, 'HEAD~2'),
	'commit_cache_hits=2 commit_cache_misses=0 commits_read=0',
	'another backend finds what the first read');

# Dropping a database forgets its entries.
$node->safe_psql('postgres', 'CREATE DATABASE gone');
$node->safe_psql('gone', q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'g0');
	INSERT INTO s.t VALUES (1);
	SELECT dolt_commit('-am', 'g1');
	SELECT dolt_hashof('HEAD~1');
});
my ($dboid, $generation, $head) = split /\|/, $node->safe_psql('gone', q{
	SELECT (SELECT oid FROM pg_database WHERE datname = 'gone'),
		test_chunk_generation(), dolt_hashof('HEAD')});
my $has = qq{SELECT test_commit_cache_has($dboid, $generation, '$head')};
is($node->safe_psql('cc', $has), 't', 'a database\'s commit is in the cache');
$node->safe_psql('postgres', 'DROP DATABASE gone');
is($node->safe_psql('cc', $has), 'f', 'and forgotten once the database is dropped');

# A commit no ref names any more, which a collection drops.
my $loose = $node->safe_psql('cc', q{
	SELECT dolt_branch('loose');
	SELECT dolt_checkout('loose');
	UPDATE s.t SET v = 3;
	SELECT dolt_commit('-am', 'loose');
	SELECT dolt_hashof('HEAD');
}) =~ s/.*\n//sr;
$node->safe_psql('cc', q{SELECT dolt_branch('-D', 'loose')});
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

# A standby's sessions fill its cache.
is(walk($standby, 'HEAD~2'),
	'commit_cache_hits=0 commit_cache_misses=2 commits_read=2',
	'a standby\'s first walk reads its commits');
is(walk($standby, 'HEAD~2'),
	'commit_cache_hits=2 commit_cache_misses=0 commits_read=0',
	'another standby backend finds what the first read');
is($standby->safe_psql('cc', qq{SELECT dolt_hashof('$loose')}), $loose,
	'the loose commit resolves on the standby, read into its cache');
is(walk($standby, $loose),
	'commit_cache_hits=1 commit_cache_misses=0 commits_read=0',
	'and is found there');

# A collection, replayed: every entry let go of, the loose commit gone.
$node->safe_psql('cc', 'SELECT dolt_gc()');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
my ($ret, $out, $err) = $standby->psql('cc', qq{SELECT dolt_hashof('$loose')});
like($err, qr/does not exist/, 'after the replayed collection the loose commit does not resolve');
is(walk($standby, 'HEAD~2'),
	'commit_cache_hits=0 commit_cache_misses=2 commits_read=2',
	'and a walk reads its commits again');

done_testing();
