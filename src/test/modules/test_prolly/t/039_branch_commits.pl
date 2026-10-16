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

# Two committers on one branch: the one that waited for the other builds on
# its commit with its rows (read committed: a newer snapshot), or, if its
# snapshot can't move (repeatable read), fails to be retried, never
# committing a root without them on top of it.
sql('CREATE EXTENSION test_prolly');
sub waits_for
{
	my ($what) = @_;
	return $node->poll_query_until('postgres', qq{
		SELECT count(*) > 0 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		WHERE NOT l.granted AND a.query LIKE '%$what%'});
}
my $first = $node->background_psql('postgres');
my $second = $node->background_psql('postgres', on_error_stop => 0);
$first->query_safe(q{BEGIN; INSERT INTO app.t VALUES (10, 'first');
	SELECT dolt_commit('-m', 'first of two')});
$second->query_until(qr/started/, q{
	\echo started
	INSERT INTO app.t VALUES (11, 'second');
	SELECT dolt_commit('-m', 'second of two');
	\echo committed
});
ok(waits_for('second of two'), 'a second committer on the branch waits');
$first->query_safe('COMMIT');
$second->query_until(qr/committed/, '');
is(sql(q{SELECT string_agg(message, ',') FROM dolt_log('main')}),
	'second of two,first of two,on main,first', 'it builds on the first');
is(sql(q{SELECT test_fold_root() = prolly_root_hash('main')}), 't',
	'with the first one\'s rows');

$second->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$first->query_safe(q{BEGIN; INSERT INTO app.t VALUES (12, 'first');
	SELECT dolt_commit('-m', 'first again')});
$second->query_until(qr/started/, q{
	\echo started
	SELECT dolt_commit('--allow-empty', '-m', 'second again');
	\echo done
});
ok(waits_for('second again'), 'a repeatable read committer waits too');
$first->query_safe('COMMIT');
$second->query_until(qr/done/, '');
like($second->{stderr}, qr/could not serialize access: branch "main" moved/,
	'and, its snapshot lacking the first one\'s commit, fails');
$second->query('ROLLBACK');
is(sql(q{SELECT string_agg(message, ',') FROM dolt_log('main')}),
	'first again,second of two,first of two,on main,first', 'committing nothing');
$first->quit;
$second->quit;

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
