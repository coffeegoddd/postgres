# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's first use with no branch at its head copies the branch the
# move planner estimates is nearest, here not the session's.  A writer of
# that branch in the middle of a change (idle in its transaction, maybe for
# good) doesn't hold the first use up, nor, through the refs it holds,
# commits to the session's branch: the session's branch is copied instead.
# Once that writer is done, the branch is copied, and the copy, moved to
# the head, has none of what the writer committed.  A repeatable-read
# transaction whose snapshot doesn't see that writer's commit is refused,
# to be retried.  What the first uses made survives a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('near_source_writes');
$node->init;
# (the move planner's sources, not pages kept as of the head, which come
# before them and move nothing: t/203 and t/204 test those)
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly.kept_commits = 0");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	INSERT INTO app.t SELECT i, 'v' || i FROM generate_series(1, 5000) i;
	SELECT dolt_commit('-Am', 'base')});
my $target = sql(q{SELECT dolt_hashof('HEAD')});
sql(qq{SELECT dolt_branch('near');
	SELECT dolt_branch('t1', '$target');
	SELECT dolt_branch('t2', '$target');
	SELECT dolt_branch('t3', '$target')});
$node->safe_psql('postgres/near', q{UPDATE app.t SET v = 'near' WHERE id <= 20;
	SELECT dolt_commit('-am', 'near')});
sql(q{UPDATE app.t SET v = 'far' WHERE id % 2 = 0;
	SELECT dolt_commit('-am', 'far')});

my $as_committed = q{SELECT count(*) FILTER (WHERE v = 'v' || id) || '/' || count(*)
	FROM app.t};

my $writer = $node->background_psql('postgres/near');
my $user = $node->background_psql('postgres', on_error_stop => 0);

# (the user's counts flushed first)
sub counter
{
	$user->query_safe(q{SELECT pg_stat_force_next_flush()});
	return $user->query_safe(qq{SELECT value FROM pg_stat_prolly WHERE name = '$_[0]'});
}

# near's writer in flight, idle in its transaction: t1's first use doesn't
# wait for it (statement_timeout would cancel it), copying main instead,
# and a commit to main meanwhile goes through.
$writer->query_safe(q{BEGIN; UPDATE app.t SET v = 'written' WHERE id <= 40});
my $busy = counter('first_use_sources_busy');
$user->query_safe(q{SET statement_timeout = '30s'});
$user->query(q{SELECT dolt_checkout('t1')});
is($user->{stderr}, '', 'the first use doesn\'t wait for near\'s writer');
is(counter('first_use_sources_busy'), $busy + 1, 'it copied main instead');
is($user->query_safe($as_committed), '5000/5000', 'moved to the head');
is($user->query_safe(q{SELECT count(*) FROM dolt_status}), '0', 'clean');
$node->safe_psql('postgres', q{SET statement_timeout = '30s';
	UPDATE app.t SET v = 'far2' WHERE id = 2; SELECT dolt_commit('-am', 'meanwhile')});
is(sql(q{SELECT v FROM app.t WHERE id = 2}), 'far2', 'a commit to main meanwhile goes through');
$writer->query_safe('COMMIT');
# (moved far from the head before it is left, as each branch used here
# is: else the next first use copies it, at the head)
my $apart = q{UPDATE app.t SET v = 'apart' WHERE id > 1000;
	SELECT dolt_commit('-am', 'apart');
	SELECT dolt_checkout('main')};
$user->query_safe($apart);

# near's writer done: t3's first use copies near, its 20 rows and the
# writer's 40 written back, not main's 2500.
my $rows = counter('apply_rows');
$user->query_safe(q{SELECT dolt_checkout('t3')});
is(counter('apply_rows') - $rows, '40', 'with near idle, near\'s copies');
is($user->query_safe($as_committed), '5000/5000',
	'the copies, near\'s with its writer\'s rows, moved to the head');
$user->query_safe($apart);
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
is($node->safe_psql('postgres/t3', $as_committed), '1000/5000', 'and t3');
is($node->safe_psql('postgres/near', q{SELECT count(*) FROM app.t WHERE v IN ('written', 'later')}),
	'40', 'and near');

done_testing();
