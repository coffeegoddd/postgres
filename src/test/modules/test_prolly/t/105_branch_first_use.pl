# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's first use copies the session branch's pages once its writers
# are done, so the copy holds rows they committed after the session's
# snapshot, and the old versions of rows they deleted or updated.  The move
# to the branch's head must leave the branch as its head has it, both as
# the session sees it and as everyone sees it once the session commits:
# read committed, by a snapshot that sees the copies; repeatable read and
# serializable, whose snapshot can't move, fail with a serialization
# failure when the copies hold what the snapshot doesn't see, and the
# retry, by a new snapshot, checks the branch out as its head.  Commits
# the copies don't hold (to a table outside the versioned schemas) don't
# stop it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('first_use');
$node->init;
# (first uses that copy the source's storage: one made of pages kept as of
# its head holds none of the rows written since, nor waits for writers;
# t/203_first_use_kept_snapshots.pl tests those)
$node->append_conf('postgresql.conf', "autovacuum = off\ndebug_prolly_page_roots = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Each branch's head is 1:1, 2:2; main's head is 1:5, 2:2, 3:3.  (Values
# out of line too, in the TOAST table.)
my @branches = qw(rr rrs rrn rrd rrw rrbig rc conn);
sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text);
	ALTER TABLE app.t ALTER pad SET STORAGE EXTERNAL;
	INSERT INTO app.t VALUES (1, 1, repeat('x', 5000)), (2, 2, NULL);
	CREATE TABLE app.b1 (id int PRIMARY KEY, v int, pad text);
	CREATE INDEX ON app.b1 (v);
	ALTER TABLE app.b1 ALTER pad SET STORAGE EXTERNAL;
	INSERT INTO app.b1 SELECT i, i % 7, CASE WHEN i % 100 = 0
		THEN repeat('y', 3000) ELSE repeat('z', 50) END
		FROM generate_series(1, 1000) i;
	CREATE TABLE app.far (id int PRIMARY KEY);
	INSERT INTO app.far SELECT generate_series(1, 3000);
	CREATE TABLE app.b20 (LIKE app.b1 INCLUDING ALL);
	INSERT INTO app.b20 SELECT i, i % 7, CASE WHEN i % 1000 = 0
		THEN repeat('y', 3000) ELSE NULL END
		FROM generate_series(1, 20000) i;
	SELECT dolt_commit('-Am', 'c1');
} . join('', map { "SELECT dolt_branch('$_');\n" } @branches) . q{
	UPDATE app.t SET v = 5 WHERE id = 1;
	INSERT INTO app.t VALUES (3, 3);
	SELECT dolt_commit('-am', 'c2');
});

my $writer = $node->background_psql('postgres');
my $user = $node->background_psql('postgres', on_error_stop => 0);

my $rows = q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t};

# A branch used, made to differ from its head before it is left, and to be
# further from it than main (far's rows deleted): a first use copies the
# branch nearest its head (#491), or estimated cheapest to move there
# (#715), and each case here is of copies of main, waiting for its
# writers.
sub apart
{
	$user->query_safe(q{INSERT INTO app.t VALUES (77, 77); DELETE FROM app.far;
		SELECT dolt_commit('-am', 'apart')});
}

sub rows_of
{
	my ($s) = @_;
	return $s->query_safe($rows) . ' ' .
	  $s->query_safe(q{SELECT count(*) FROM dolt_status});
}

# A repeatable read (or serializable) first use, after the writer's
# change: refused, then retried.
sub refused_then_retried
{
	my ($iso, $checkout, $change, $expected, $what) = @_;

	$user->query_safe("BEGIN ISOLATION LEVEL $iso; SELECT 1");
	$writer->query_safe($change);
	my $ret = $user->query("SELECT $checkout");
	like($user->{stderr}, qr/could not serialize access: the branch's first use/,
		"$what: refused");
	$user->{stderr} = '';	# (query() keeps it till cleared)
	$user->query_safe('ROLLBACK');
	$user->query_safe("BEGIN ISOLATION LEVEL $iso; SELECT 1");
	$user->query_safe("SELECT $checkout");
	is(rows_of($user), $expected, "$what: retried, the branch as its head has it");
	$user->query_safe('COMMIT');
	is(rows_of($user), $expected, "$what: and after the commit");
	apart();
	$user->query_safe(q{SELECT dolt_checkout('main')});
	return;
}

# (a first use tries its locks again until it gets them, not queued)
sub wait_for_lock
{
	my ($query) = @_;
	return $node->poll_query_until('postgres', qq{
		SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'BranchCopyLock' AND query LIKE '%$query%'});
}

# Repeatable read: another session commits after the snapshot.  The
# first use is refused; retried, the branch is its head, clean, in the
# transaction and after it.
refused_then_retried('REPEATABLE READ', q{dolt_checkout('rr')},
	q{INSERT INTO app.t VALUES (4, 4); DELETE FROM app.t WHERE id = 2},
	'1:1,2:2 0', 'a repeatable read first use whose copy holds rows its snapshot lacks');
is(sql(q{SELECT length(pad) FROM app.t WHERE id = 1 AND current_setting('search_path') IS NOT NULL}),
	'5000', 'main\'s values out of line');
$user->query_safe(q{SELECT dolt_checkout('rr')});
is($user->query_safe(q{SELECT length(pad) FROM app.t WHERE id = 1}),
	'5000', 'the branch\'s values out of line');
$user->query_safe(q{SELECT dolt_checkout('main')});
is(sql($rows), '1:5,3:3,4:4', 'main keeps the other session\'s changes');

# The same with -b, a branch made in the transaction, at main's head.
refused_then_retried('REPEATABLE READ', q{dolt_checkout('-b', 'rrb')},
	q{INSERT INTO app.t VALUES (2, 2); DELETE FROM app.t WHERE id = 4},
	'1:5,2:2,3:3 0', 'checkout -b');

# Serializable too.
refused_then_retried('SERIALIZABLE', q{dolt_checkout('rrs')},
	q{UPDATE app.t SET v = 6 WHERE id = 1}, '1:1,2:2 0', 'serializable');
sql(q{UPDATE app.t SET v = 5 WHERE id = 1});

# A commit after the snapshot the copies don't hold (a table outside the
# versioned schemas): no refusal, and no table read in full to tell.
sql(q{CREATE TABLE plain (id int) USING heap; SELECT pg_stat_reset_shared('prolly')});
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(q{INSERT INTO plain VALUES (1)});
$user->query_safe(q{SELECT dolt_checkout('-b', 'rrp')});
is(rows_of($user), '1:5,2:2,3:3 0', 'a commit elsewhere doesn\'t refuse the first use');
$user->query_safe('COMMIT');
apart();
$user->query_safe(q{SELECT dolt_checkout('main')});
sql(q{SELECT pg_stat_force_next_flush()});
is(sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'first_use_tables_scanned'}),
	'0', 'the change logs told, no table read in full');

# Changes after the snapshot that leave the rows as they were: the old
# versions, which the snapshot sees, are gone for everyone else.  The
# branch, committed to, gets its own rows, not main's.
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(q{UPDATE app.t SET v = v WHERE id = 1});
$user->query(q{SELECT dolt_checkout('rrn')});
like($user->{stderr}, qr/could not serialize access/,
	'a no-op update after the snapshot: refused');
$user->{stderr} = '';	# (query() keeps it till cleared)
$user->query_safe('ROLLBACK');
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$user->query_safe(q{SELECT dolt_checkout('rrn')});
is(rows_of($user), '1:1,2:2 0', 'and retried');
$user->query_safe(q{INSERT INTO app.t VALUES (77, 77); DELETE FROM app.far;
	SELECT dolt_commit('-am', 'on rrn')});
is(rows_of($user), '1:1,2:2,77:77 0', 'committed to, clean');
$user->query_safe('COMMIT');
is(rows_of($user), '1:1,2:2,77:77 0', 'and after the commit');
$user->query_safe(q{SELECT dolt_reset('--hard')});
is(rows_of($user), '1:1,2:2,77:77 0', 'its commit has the branch\'s rows');
$user->query_safe(q{SELECT dolt_checkout('main')});

refused_then_retried('REPEATABLE READ', q{dolt_checkout('rrd')},
	q{DELETE FROM app.t WHERE id = 3; INSERT INTO app.t VALUES (3, 3)},
	'1:1,2:2 0', 'a delete and re-insert after the snapshot');
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
like($user->{stderr}, qr/could not serialize access/,
	'and is refused: the writer committed after the snapshot');
$user->{stderr} = '';	# (query() keeps it till cleared)
$user->query_safe('ROLLBACK');
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$user->query_safe(q{SELECT dolt_checkout('rrw')});
is(rows_of($user), '1:1,2:2 0', 'retried, the branch as its head has it, clean');
$user->query_safe('COMMIT');
is(rows_of($user), '1:1,2:2 0', 'and after the commit');
apart();
$user->query_safe(q{SELECT dolt_checkout('main')});
is(sql($rows), '1:5,2:7,3:3', 'main keeps the writer\'s change');
sql(q{UPDATE app.t SET v = 2 WHERE id = 2});

# Tables of many pages, with values out of line and a second index, that
# commits after the snapshot changed: refused, and retried, every row.
my $big = join(" || ' / ' || ", map { qq{(SELECT format('%s %s %s %s %s',
	count(*), min(id), max(id), sum(v), sum(length(pad))) FROM app.$_)} } qw(b1 b20));
$big = "SELECT $big";
my $big_head = sql($big);
my $by_index = q{SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT (SELECT count(*) FROM app.b1 WHERE v = 3) || ' ' ||
	(SELECT count(*) FROM app.b20 WHERE v = 3) || ' ' ||
	(SELECT count(*) FROM app.b20 WHERE id > 0)};
my $by_index_head = sql($by_index);
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(q{INSERT INTO app.b1 VALUES (-1, 3, 'x');
	DELETE FROM app.b20 WHERE id = 500});
$user->query(q{SELECT dolt_checkout('rrbig')});
like($user->{stderr}, qr/could not serialize access/, 'tables of many pages: refused');
$user->{stderr} = '';	# (query() keeps it till cleared)
$user->query_safe('ROLLBACK');
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$user->query_safe(q{SELECT dolt_checkout('rrbig')});
is($user->query_safe($big), $big_head, 'tables of many pages, every row');
is($user->query_safe($by_index), $by_index_head, 'and by their indexes');
is($user->query_safe(q{SELECT count(*) FROM dolt_status}), '0', 'clean');
$user->query_safe(q{INSERT INTO app.b1 VALUES (5000, 0, 'w'); DELETE FROM app.far;
	SELECT dolt_commit('-am', 'on rrbig')});
$user->query_safe('COMMIT');
$user->query_safe(q{SELECT dolt_reset('--hard')});
is($user->query_safe(q{SELECT count(*) FROM app.b1}) . ' ' .
	$user->query_safe(q{SELECT count(*) FROM app.b20}) . ' ' .
	$user->query_safe(q{SELECT count(*) FROM dolt_status}), '1001 20000 0',
	'its commit has every row, and one more');
$user->query_safe(q{SELECT dolt_checkout('main')});
is(sql(q{SELECT count(*) FROM app.b1}) . ' ' . sql(q{SELECT count(*) FROM app.b20}),
	'1001 19999', 'main keeps the other session\'s changes');

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
apart();
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
$user->query_safe(q{DELETE FROM app.far; SELECT dolt_commit('-am', 'apart')});
$user->query_safe(q{SELECT dolt_checkout('main')});
is(sql($rows), '1:5,3:3,102:102', 'main keeps the writer\'s rows');

# A connection to a branch not used yet, its role's default repeatable
# read, while a writer on main is in flight: it waits, and connects.
sql(q{ALTER ROLE CURRENT_USER SET default_transaction_isolation = 'repeatable read'});
$writer->query_safe(q{BEGIN; INSERT INTO app.t VALUES (103, 103)});
my $conn = $node->background_psql('postgres/conn', wait => 0);
ok($node->poll_query_until('postgres', q{
	SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'BranchCopyLock'}),
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

# The branches' copies as recovery replays them: by a sequential scan and
# by the primary key.
$node->stop('immediate');
$node->start;
my %after = (rr => '1:1,2:2,77:77', rrs => '1:1,2:2,77:77',
	rrn => '1:1,2:2,77:77', rrd => '1:1,2:2,77:77', rrw => '1:1,2:2,77:77',
	rrb => '1:5,2:2,3:3,77:77');
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
is($node->safe_psql('postgres/rrbig', q{SELECT count(*) FROM app.b1}) . ' ' .
	$node->safe_psql('postgres/rrbig', $by_index) . ' ' .
	$node->safe_psql('postgres/rrbig', q{SELECT count(*) FROM dolt_status}),
	"1001 $by_index_head 0", 'and the tables of many pages');

done_testing();
