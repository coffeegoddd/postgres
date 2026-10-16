# Copyright (c) 2026, PostgreSQL Global Development Group

# Commits of branches made by copying: committers on different branches
# take turns on the refs and each keeps the other's commit; commits survive
# a crash and reach a standby.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('commits');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('commits_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	INSERT INTO app.t VALUES (1, 'a');
	SELECT dolt_commit('-m', 'first');
	SELECT dolt_branch('f');
});

# A committer on f waits for one on main, then builds on its refs.
my $a = $node->background_psql('postgres');
my $b = $node->background_psql('postgres');
$b->query_safe(q{SELECT dolt_checkout('f')});
$a->query_safe(q{BEGIN; INSERT INTO app.t VALUES (2, 'main');
	SELECT dolt_commit('-m', 'on main')});
$b->query_until(qr/started/, q{
	\echo started
	INSERT INTO app.t VALUES (2, 'f');
	SELECT dolt_commit('-m', 'on f');
	\echo committed
});
ok($node->poll_query_until('postgres', q{
	SELECT count(*) > 0 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
	WHERE NOT l.granted AND a.query LIKE '%on f%'}),
	'a committer on another branch waits for the refs');
$a->query_safe('COMMIT');
$b->query_until(qr/committed/, '');
is(sql(q{SELECT string_agg(message, ',') FROM dolt_log('main')}), 'on main,first',
	'main has its commit');
is(sql(q{SELECT string_agg(message, ',') FROM dolt_log('f')}), 'on f,first',
	'and f its own');
$a->quit;
$b->quit;

# A crash keeps them, and a standby has them.
my $heads = q{SELECT string_agg(name || ':' || coalesce(hash, '') || ':' ||
	latest_commit_message, ',' ORDER BY name) FROM dolt_branches()};
my $before = sql($heads);
$node->stop('immediate');
$node->start;
is(sql($heads), $before, 'commits survive a crash');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', q{SELECT string_agg(message, ',') FROM dolt_log('f')}),
	'on f,first', 'and reach a standby');
is($standby->safe_psql('postgres', q{SELECT prolly_root_hash('main')}),
	sql(q{SELECT prolly_root_hash('main')}), 'with the same roots');

$standby->stop;
$node->stop;
done_testing();
