# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's first use copies the session branch once no writer is in the
# middle of changing it, but doesn't queue for its locks: a writer that
# comes while it waits goes ahead of it, rather than behind it.  With
# lock_timeout set, the first use gives up after that long, with a hint.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('first_use_lock');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int);
	INSERT INTO app.t SELECT i, i FROM generate_series(1, 10) i;
	SELECT dolt_commit('-Am', 'c1');
	SELECT dolt_branch('b1');
	SELECT dolt_branch('b2')});

my $rows = q{SELECT string_agg(id || ':' || v, ',' ORDER BY id)
	FROM app.t WHERE id IN (1, 2, 3)};

my $writer = $node->background_psql('postgres');
my $user = $node->background_psql('postgres', on_error_stop => 0);
my $other = $node->background_psql('postgres', on_error_stop => 0);

# A writer in flight on main; the first use of b1 waits for it.
$writer->query_safe(q{BEGIN; UPDATE app.t SET v = 100 WHERE id = 1});
$user->query_until(qr/started/, q{
	\echo started
	SELECT dolt_checkout('b1');
	\echo done
});
my $first_use_wait = q{SELECT wait_event FROM pg_stat_activity
	WHERE query LIKE '%dolt_checkout(''b1'')%' AND pid <> pg_backend_pid()};
ok( $node->poll_query_until('postgres', qq{
		SELECT wait_event_type IN ('Lock', 'Timeout') FROM pg_stat_activity
		WHERE query LIKE '%dolt_checkout(''b1'')%' AND pid <> pg_backend_pid()}),
	'the first use waits for the writer');
is(sql($first_use_wait), 'BranchCopyLock', 'trying the locks again');
is( sql(q{SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		WHERE NOT l.granted AND a.query LIKE '%dolt_checkout(''b1'')%'}),
	'0', 'not queued for them');

# Another writer on main meanwhile goes ahead, not behind the first use.
$other->query_safe(q{SET lock_timeout = '5s'});
$other->query(q{UPDATE app.t SET v = 200 WHERE id = 2});
is($other->{stderr}, '', 'a writer that comes after it is not held up');
is(sql($first_use_wait), 'BranchCopyLock', 'while the first use still waits');

$writer->query_safe('COMMIT');
$user->query_until(qr/done/, '');
is($user->{stderr}, '', 'once the writer is done, the first use goes on');
is($user->query_safe($rows), '1:1,2:2,3:3', 'and the branch is its head');
# (made to differ from its head before it is left: a first use copies the
# branch nearest its head, and b2's must be of main's copies, behind main's
# writer)
$user->query_safe(q{INSERT INTO app.t VALUES (77, 77);
	SELECT dolt_commit('-am', 'apart')});
$user->query_safe(q{SELECT dolt_checkout('main')});
is(sql($rows), '1:100,2:200,3:3', 'main keeps both writers\' changes');

# With lock_timeout, the first use gives up, with a hint.
$writer->query_safe(q{BEGIN; UPDATE app.t SET v = 300 WHERE id = 3});
$user->query_safe(q{SET lock_timeout = '200ms'});
$user->query(q{SELECT dolt_checkout('b2')});
like($user->{stderr},
	qr/could not lock relation "t" of branch "main" to copy it/,
	'with lock_timeout, a first use behind a writer gives up');
like($user->{stderr}, qr/HINT:  Try again once the branch's writers are done, or raise "lock_timeout"/,
	'with a hint');
$user->{stderr} = '';
is($user->query_safe(q{SELECT active_branch()}), 'main', 'still on main');
$writer->query_safe('COMMIT');

$user->query(q{SELECT dolt_checkout('b2')});
is($user->{stderr}, '', 'tried again once the writer is done, it checks out');
is($user->query_safe($rows), '1:1,2:2,3:3', 'the branch as its head has it');

# The page store is synced once the locks are let go, at commit: a writer
# then goes ahead too.
SKIP:
{
	skip 'injection points not supported by this build', 4
	  unless $ENV{enable_injection_points} eq 'yes';

	sql(q{CREATE EXTENSION injection_points;
		SELECT dolt_branch('b3');
		SELECT injection_points_attach('bmap-sync-shares', 'wait')});
	$user->query_until(qr/started/, q{
		\echo started
		SELECT dolt_checkout('b3');
		\echo done
	});
	my $b3 = q{SELECT wait_event FROM pg_stat_activity
		WHERE query LIKE '%dolt_checkout(''b3'')%' AND pid <> pg_backend_pid()};
	$node->poll_query_until('postgres', qq{SELECT state = 'idle'
		OR wait_event = 'bmap-sync-shares' FROM pg_stat_activity
		WHERE query LIKE '%dolt_checkout(''b3'')%' AND pid <> pg_backend_pid()});
	is(sql($b3), 'bmap-sync-shares', 'the first use syncs the page store at commit');
	$other->query(q{UPDATE app.t SET v = 400 WHERE id = 3});
	is($other->{stderr}, '', 'a writer meanwhile is not held up');
	sql(q{SELECT injection_points_detach('bmap-sync-shares');
		SELECT injection_points_wakeup('bmap-sync-shares')});
	$user->query_until(qr/done/, '');
	is($user->{stderr}, '', 'and the first use commits');
	is($user->query_safe($rows), '1:1,2:2,3:3', 'the branch as its head has it');
	$user->query_safe(q{SELECT dolt_checkout('main')});
}

$user->quit;
$other->quit;
$writer->quit;

# What the first uses shared survives a crash.
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres/b1', $rows), '1:1,2:2,3:3', 'after a crash, b1');
is($node->safe_psql('postgres/b2', $rows), '1:1,2:2,3:3', 'and b2');
is(sql($rows),
	$ENV{enable_injection_points} eq 'yes' ? '1:100,2:200,3:400' : '1:100,2:200,3:300',
	'and main');
is($node->safe_psql('postgres/b3', $rows), '1:1,2:2,3:3', 'and b3')
  if $ENV{enable_injection_points} eq 'yes';

done_testing();
