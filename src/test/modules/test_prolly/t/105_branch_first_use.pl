# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's first use copies the session branch's pages once its writers
# are done, so the copy holds rows they committed after the session's
# snapshot.  The move to the branch's head must undo those too: read
# committed, by a snapshot that sees them; where the snapshot can't move
# (repeatable read), it fails to be retried instead.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('first_use');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY);
	INSERT INTO app.t VALUES (1), (2);
	SELECT dolt_commit('-Am', 'c1');
	SELECT dolt_branch('rr');
	SELECT dolt_branch('rc');
});

my $writer = $node->background_psql('postgres');
my $user = $node->background_psql('postgres', on_error_stop => 0);

sub on_branch
{
	my ($s) = @_;
	return $s->query_safe(q{SELECT array_agg(id ORDER BY id) FROM app.t}) .
		' ' . $s->query_safe(q{SELECT count(*) FROM dolt_status});
}

# Repeatable read: another session commits after the snapshot.
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(q{INSERT INTO app.t VALUES (3); DELETE FROM app.t WHERE id = 1});
$user->query(q{SELECT dolt_checkout('rr')});
like($user->{stderr}, qr/could not serialize access/,
	'a repeatable read first use whose copy holds rows its snapshot lacks fails');
$user->query('ROLLBACK');
$user->{stderr} = '';
is($user->query_safe(q{SELECT active_branch()}), 'main', 'staying on main');

# Retried, it gets the branch as its head has it, clean.
$user->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT dolt_checkout('rr')});
is(on_branch($user), '{1,2} 0', 'retried, the branch is its head, clean');
$user->query_safe('COMMIT');
is(on_branch($user), '{1,2} 0', 'and after the commit');
$user->query_safe(q{SELECT dolt_checkout('main')});
is(on_branch($user), '{2,3} 1', 'main keeps the other session\'s changes');

# The same with -b, for a branch made in the transaction.
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(q{INSERT INTO app.t VALUES (4)});
$user->query(q{SELECT dolt_checkout('-b', 'rrb')});
like($user->{stderr}, qr/could not serialize access/, 'so does checkout -b');
$user->query('ROLLBACK');
$user->{stderr} = '';

# Read committed: the first use waits for a writer, whose rows the
# statement's snapshot, taken before, doesn't see.
$writer->query_safe(q{BEGIN; INSERT INTO app.t VALUES (101);
	DELETE FROM app.t WHERE id = 2});
$user->query_until(qr/started/, q{
	\echo started
	SELECT dolt_checkout('rc');
	\echo done
});
ok($node->poll_query_until('postgres', q{
	SELECT count(*) > 0 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
	WHERE NOT l.granted AND a.query LIKE '%dolt_checkout(''rc'')%'}),
	'a read committed first use waits for the writer');
$writer->query_safe('COMMIT');
$user->query_until(qr/done/, '');
is($user->{stderr}, '', 'and checks out');
is(on_branch($user), '{1,2} 0', 'the branch as its head has it, clean');
$user->query_safe(q{SELECT dolt_checkout('main')});
is(on_branch($user), '{3,4,101} 1', 'main keeps the writer\'s rows');

# A first use in a transaction block, read committed.
$user->query_safe(q{BEGIN; SELECT 1});
$writer->query_safe(q{INSERT INTO app.t VALUES (102)});
$user->query_safe(q{SELECT dolt_checkout('-b', 'rcb')});
is(on_branch($user), '{1,2} 0', 'one made in a transaction block too');
$user->query_safe('COMMIT');
is(on_branch($user), '{1,2} 0', 'and after the commit');

$user->quit;
$writer->quit;
done_testing();
