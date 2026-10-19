# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's first use copies the session branch's pages once its writers
# are done, so the copy holds rows they committed after the session's
# snapshot, and the old versions of rows they deleted or updated.  The move
# to the branch's head must leave the branch as its head has it, both as
# the session sees it and as everyone sees it once the session commits:
# read committed, by a snapshot that sees the copies; repeatable read and
# serializable, by emptying the copies that hold what the snapshot doesn't
# see, for the move to write their rows.

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

# Each branch's head is 1:1, 2:2; main's head is 1:5, 2:2, 3:3.  (Values
# out of line too, in the TOAST table.)
my @branches = qw(rr rrs rrn rrd rrw rc conn);
sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text);
	ALTER TABLE app.t ALTER pad SET STORAGE EXTERNAL;
	INSERT INTO app.t VALUES (1, 1, repeat('x', 5000)), (2, 2, NULL);
	SELECT dolt_commit('-Am', 'c1');
} . join('', map { "SELECT dolt_branch('$_');\n" } @branches) . q{
	UPDATE app.t SET v = 5 WHERE id = 1;
	INSERT INTO app.t VALUES (3, 3);
	SELECT dolt_commit('-am', 'c2');
});

my $writer = $node->background_psql('postgres');
my $user = $node->background_psql('postgres', on_error_stop => 0);

my $rows = q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t};

sub rows_of
{
	my ($s) = @_;
	return $s->query_safe($rows) . ' ' .
	  $s->query_safe(q{SELECT count(*) FROM dolt_status});
}

sub wait_for_lock
{
	my ($query) = @_;
	return $node->poll_query_until('postgres', qq{
		SELECT count(*) > 0 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		WHERE NOT l.granted AND a.query LIKE '%$query%'});
}

# Repeatable read: another session commits after the snapshot.  The
# first use succeeds, and the branch is its head, clean, in the
# transaction and after it.
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(
	q{INSERT INTO app.t VALUES (4, 4); DELETE FROM app.t WHERE id = 2});
$user->query_safe(q{SELECT dolt_checkout('rr')});
is(rows_of($user), '1:1,2:2 0',
	'a repeatable read first use whose copy holds rows its snapshot lacks');
is($user->query_safe(q{SELECT length(pad) FROM app.t WHERE id = 1}),
	'5000', 'with its values out of line');
$user->query_safe('COMMIT');
is(rows_of($user), '1:1,2:2 0', 'and after the commit');
$user->query_safe(q{SELECT dolt_checkout('main')});
is(sql($rows), '1:5,3:3,4:4', 'main keeps the other session\'s changes');

# The same with -b, a branch made in the transaction, at main's head.
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(
	q{INSERT INTO app.t VALUES (2, 2); DELETE FROM app.t WHERE id = 4});
$user->query_safe(q{SELECT dolt_checkout('-b', 'rrb')});
is(rows_of($user), '1:5,2:2,3:3 0', 'checkout -b');
$user->query_safe('COMMIT');
is(rows_of($user), '1:5,2:2,3:3 0', 'and after the commit');
$user->query_safe(q{SELECT dolt_checkout('main')});

# Serializable too.
$user->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT 1');
$writer->query_safe(q{UPDATE app.t SET v = 6 WHERE id = 1});
$user->query_safe(q{SELECT dolt_checkout('rrs')});
is(rows_of($user), '1:1,2:2 0', 'serializable');
$user->query_safe('COMMIT');
is(rows_of($user), '1:1,2:2 0', 'and after the commit');
$user->query_safe(q{SELECT dolt_checkout('main')});
sql(q{UPDATE app.t SET v = 5 WHERE id = 1});

# Changes after the snapshot that leave the rows as they were: the old
# versions, which the snapshot sees, are gone for everyone else.  The
# branch, committed to, gets its own rows, not main's.
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(q{UPDATE app.t SET v = v WHERE id = 1});
$user->query_safe(q{SELECT dolt_checkout('rrn')});
is(rows_of($user), '1:1,2:2 0', 'a no-op update after the snapshot');
$user->query_safe(q{INSERT INTO app.t VALUES (77, 77);
	SELECT dolt_commit('-am', 'on rrn')});
is(rows_of($user), '1:1,2:2,77:77 0', 'committed to, clean');
$user->query_safe('COMMIT');
is(rows_of($user), '1:1,2:2,77:77 0', 'and after the commit');
$user->query_safe(q{SELECT dolt_reset('--hard')});
is(rows_of($user), '1:1,2:2,77:77 0', 'its commit has the branch\'s rows');
$user->query_safe(q{SELECT dolt_checkout('main')});

$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(
	q{DELETE FROM app.t WHERE id = 3; INSERT INTO app.t VALUES (3, 3)});
$user->query_safe(q{SELECT dolt_checkout('rrd')});
is(rows_of($user), '1:1,2:2 0', 'a delete and re-insert after the snapshot');
$user->query_safe('COMMIT');
is(rows_of($user), '1:1,2:2 0', 'and after the commit');
$user->query_safe(q{SELECT dolt_checkout('main')});
is(sql($rows), '1:5,2:2,3:3', 'main as it was');

# Repeatable read, a writer in flight when the snapshot is taken, which
# the first use waits for: as under a steady load of writes, every try.
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(q{BEGIN; UPDATE app.t SET v = 7 WHERE id = 2});
$user->query_until(qr/started/, q{
	\echo started
	SELECT dolt_checkout('rrw');
	\echo done
});
ok(wait_for_lock(q{dolt_checkout(''rrw'')}),
	'a repeatable read first use waits for the writer');
$writer->query_safe('COMMIT');
$user->query_until(qr/done/, '');
is($user->{stderr}, '', 'and checks out');
is(rows_of($user), '1:1,2:2 0', 'the branch as its head has it, clean');
$user->query_safe('COMMIT');
is(rows_of($user), '1:1,2:2 0', 'and after the commit');
$user->query_safe(q{SELECT dolt_checkout('main')});
is(sql($rows), '1:5,2:7,3:3', 'main keeps the writer\'s change');
sql(q{UPDATE app.t SET v = 2 WHERE id = 2});

# Read committed: the first use waits for a writer, whose rows the
# statement's snapshot, taken before, doesn't see.
$writer->query_safe(q{BEGIN; INSERT INTO app.t VALUES (101, 101);
	DELETE FROM app.t WHERE id = 2});
$user->query_until(qr/started/, q{
	\echo started
	SELECT dolt_checkout('rc');
	\echo done
});
ok(wait_for_lock(q{dolt_checkout(''rc'')}),
	'a read committed first use waits for the writer');
$writer->query_safe('COMMIT');
$user->query_until(qr/done/, '');
is($user->{stderr}, '', 'and checks out');
is(rows_of($user), '1:1,2:2 0', 'the branch as its head has it, clean');
$user->query_safe(q{SELECT dolt_checkout('main')});
is(sql($rows), '1:5,3:3,101:101', 'main keeps the writer\'s rows');

# The same in a transaction block, with -b, the writer committing while
# the first use waits for it.
$user->query_safe(q{BEGIN; SELECT 1});
$writer->query_safe(q{BEGIN; INSERT INTO app.t VALUES (102, 102);
	DELETE FROM app.t WHERE id = 101});
$user->query_until(qr/started/, q{
	\echo started
	SELECT dolt_checkout('-b', 'rcb');
	\echo done
});
ok(wait_for_lock(q{dolt_checkout(''-b'', ''rcb'')}),
	'one in a transaction block waits for the writer');
$writer->query_safe('COMMIT');
$user->query_until(qr/done/, '');
is($user->{stderr}, '', 'and checks out');
is(rows_of($user), '1:5,2:2,3:3 0', 'the branch as its head has it, clean');
$user->query_safe('COMMIT');
is(rows_of($user), '1:5,2:2,3:3 0', 'and after the commit');
$user->query_safe(q{SELECT dolt_checkout('main')});
is(sql($rows), '1:5,3:3,102:102', 'main keeps the writer\'s rows');

# A connection to a branch not used yet, its role's default repeatable
# read, while a writer on main is in flight: it waits, and connects.
sql(q{ALTER ROLE CURRENT_USER SET default_transaction_isolation = 'repeatable read'});
$writer->query_safe(q{BEGIN; INSERT INTO app.t VALUES (103, 103)});
my $conn = $node->background_psql('postgres/conn', wait => 0);
ok($node->poll_query_until('postgres', q{
	SELECT count(*) > 0 FROM pg_locks WHERE NOT granted}),
	'a connection\'s first use waits for the writer');
$writer->query_safe('COMMIT');
$conn->wait_connect;
is($conn->query_safe(q{SHOW transaction_isolation}), 'repeatable read',
	'the connection is repeatable read');
is(rows_of($conn), '1:1,2:2 0', 'and on the branch as its head has it, clean');
$conn->quit;
sql(q{ALTER ROLE CURRENT_USER RESET default_transaction_isolation});
is(sql($rows), '1:5,3:3,102:102,103:103', 'main keeps the writer\'s rows');

$user->quit;
$writer->quit;

# The emptied copies, and the rows written into them again, as recovery
# replays them: by a sequential scan and by the primary key.
$node->stop('immediate');
$node->start;
my %after = (rr => '1:1,2:2', rrs => '1:1,2:2', rrn => '1:1,2:2,77:77',
	rrd => '1:1,2:2', rrw => '1:1,2:2', rrb => '1:5,2:2,3:3');
foreach my $b (sort keys %after)
{
	is($node->safe_psql("postgres/$b", $rows), $after{$b},
		"after a crash, $b by a scan");
	is($node->safe_psql("postgres/$b", q{SET enable_seqscan = off;
		SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t
		WHERE id IN (1, 2, 3, 77)}), $after{$b}, "and by its key");
}
is($node->safe_psql('postgres/rr', q{SELECT length(pad) FROM app.t WHERE id = 1}),
	'5000', 'with its values out of line');

done_testing();
