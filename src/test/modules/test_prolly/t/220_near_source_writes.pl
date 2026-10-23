# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's first use with no branch at its head copies the branch the
# move planner estimates is nearest, here not the session's: a writer of
# that branch in flight holds the copy up, as the session's own would, and
# the copy, moved to the head once it is made, has none of what that
# writer committed.  A repeatable-read transaction whose snapshot doesn't
# see that writer's commit is refused, to be retried.  What the first use
# made survives a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('near_source_writes');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	INSERT INTO app.t SELECT i, 'v' || i FROM generate_series(1, 5000) i;
	SELECT dolt_commit('-Am', 'base')});
my $target = sql(q{SELECT dolt_hashof('HEAD')});
sql(qq{SELECT dolt_branch('near');
	SELECT dolt_branch('t1', '$target');
	SELECT dolt_branch('t2', '$target')});
$node->safe_psql('postgres/near', q{UPDATE app.t SET v = 'near' WHERE id <= 20;
	SELECT dolt_commit('-am', 'near')});
sql(q{UPDATE app.t SET v = 'far' WHERE id % 2 = 0;
	SELECT dolt_commit('-am', 'far')});

my $as_committed = q{SELECT count(*) FILTER (WHERE v = 'v' || id) || '/' || count(*)
	FROM app.t};

my $writer = $node->background_psql('postgres/near');
my $user = $node->background_psql('postgres', on_error_stop => 0);

# near's writer in flight: t1's first use, copying near, waits for it.
$writer->query_safe(q{BEGIN; UPDATE app.t SET v = 'written' WHERE id <= 40});
$user->query_until(qr/started/, q{
	\echo started
	SELECT dolt_checkout('t1');
	\echo done
});
ok( $node->poll_query_until('postgres', q{
		SELECT wait_event = 'BranchCopyLock' FROM pg_stat_activity
		WHERE query LIKE '%dolt_checkout(''t1'')%' AND pid <> pg_backend_pid()}),
	'the first use waits for near\'s writer, copying near');
$writer->query_safe('COMMIT');
$user->query_until(qr/done/, '');
is($user->{stderr}, '', 'once the writer is done, the first use goes on');
is($user->query_safe($as_committed), '5000/5000',
	'the copies, near\'s with its writer\'s rows, moved to the head');
is($user->query_safe(q{SELECT count(*) FROM dolt_status}), '0', 'clean');
# (moved far from the head before it is left: else t2's first use copies
# t1, at the head)
$user->query_safe(q{UPDATE app.t SET v = 'apart' WHERE id > 1000;
	SELECT dolt_commit('-am', 'apart');
	SELECT dolt_checkout('main')});
is($node->safe_psql('postgres/near', q{SELECT count(*) FROM app.t WHERE v = 'written'}),
	'40', 'near keeps its writer\'s rows');

# A repeatable-read snapshot from before near's next write: t2's first
# use, copying near, is refused; tried again, it checks out.
$user->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1});
$node->safe_psql('postgres/near', q{UPDATE app.t SET v = 'later' WHERE id = 1});
$user->query(q{SELECT dolt_checkout('t2')});
like($user->{stderr}, qr/could not serialize access: the branch's first use copies rows committed after this transaction's snapshot/,
	'a snapshot that doesn\'t see near\'s commit is refused');
$user->{stderr} = '';
$user->query_safe('ROLLBACK');
$user->query(q{SELECT dolt_checkout('t2')});
is($user->{stderr}, '', 'tried again, it checks out');
is($user->query_safe($as_committed), '5000/5000', 'at the head');
$user->query_safe(q{SELECT dolt_checkout('main')});

$user->quit;
$writer->quit;

$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres/t1', $as_committed), '1000/5000', 'after a crash, t1');
is($node->safe_psql('postgres/t2', $as_committed), '5000/5000', 'and t2');
is($node->safe_psql('postgres/near', q{SELECT count(*) FROM app.t WHERE v IN ('written', 'later')}),
	'40', 'and near');

done_testing();
