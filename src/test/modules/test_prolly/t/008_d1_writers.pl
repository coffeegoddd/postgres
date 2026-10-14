# Copyright (c) 2026, PostgreSQL Global Development Group

# Engine D1 with coordination "single_writer": a table has one writer at a
# time, under a lock of its own (pg_locks type prollytable).  Writers that
# both started don't deadlock; share row locks are held together while
# writers wait; NOWAIT and SKIP LOCKED see the lock; and READ COMMITTED
# follows a row a committed writer changed while it waited, as heap does,
# for tables with and without a primary key.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('writers');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method = prolly_d1\n");
$node->start;

$node->safe_psql('postgres', q{
	CREATE TABLE t (id int PRIMARY KEY, v int);
	INSERT INTO t VALUES (1, 0), (2, 0);
	CREATE TABLE k (a int, b int);
	INSERT INTO k VALUES (1, 0), (2, 0);
});

# Wait until some session waits for the right to write t.
sub wait_for_writer_wait
{
	$node->poll_query_until('postgres', q{
		SELECT count(*) > 0 FROM pg_locks
		WHERE locktype = 'prollytable' AND NOT granted
	}) or die 'no session waited for the write lock';
}

# Two transactions that both hold RowExclusiveLock on t, then both write.
my $s1 = $node->background_psql('postgres');
my $s2 = $node->background_psql('postgres', on_error_stop => 0);
$s1->query_safe('BEGIN; UPDATE t SET v = v WHERE id = -1;');
$s2->query_safe('BEGIN; UPDATE t SET v = v WHERE id = -1;');
$s1->query_safe('INSERT INTO t VALUES (3, 0)');
$s2->query_until(qr/go/, "\\echo go\nINSERT INTO t VALUES (4, 0);\n");
wait_for_writer_wait();
$s1->query_safe('COMMIT');
$s2->query_safe('COMMIT');
is($node->safe_psql('postgres', 'SELECT array_agg(id ORDER BY id) FROM t'),
	'{1,2,3,4}', 'two writers that both started commit, without a deadlock');

# Share row locks are held together; a writer waits for them.
$s1->query_safe('BEGIN; SELECT * FROM t WHERE id = 1 FOR SHARE;');
$s2->query_safe(q{BEGIN; SET LOCAL lock_timeout = '5s';
	SELECT * FROM t WHERE id = 2 FOR KEY SHARE;});
$s2->query_safe('COMMIT');
pass('FOR KEY SHARE is granted while another session holds FOR SHARE');
is($node->safe_psql('postgres', q{
	SELECT locktype, mode, granted FROM pg_locks
	WHERE locktype = 'prollytable'}),
	'prollytable|ShareLock|t', 'pg_locks shows the share lock');
my ($ret, $out, $err) = $node->psql('postgres', q{
	SET lock_timeout = '100ms'; UPDATE t SET v = 1 WHERE id = 2;});
like($err, qr/lock timeout/, 'a writer waits for share row locks');

# NOWAIT and SKIP LOCKED.
($ret, $out, $err) = $node->psql('postgres',
	'SELECT * FROM t WHERE id = 1 FOR UPDATE NOWAIT');
like($err, qr/could not obtain lock on row in relation "t"/,
	'FOR UPDATE NOWAIT fails at once');
is($node->safe_psql('postgres',
	'SELECT count(*) FROM (SELECT * FROM t FOR UPDATE SKIP LOCKED) s'),
	'0', 'FOR UPDATE SKIP LOCKED skips the locked rows');
$s1->query_safe('COMMIT');

# READ COMMITTED follows a row updated meanwhile: the second update applies
# on top of the first, for a keyed and a keyless table.
for my $case (['t', 'v', 'id'], ['k', 'b', 'a'])
{
	my ($tab, $col, $key) = @$case;

	$s1->query_safe("BEGIN; UPDATE $tab SET $col = $col + 1 WHERE $key = 1;");
	$s2->query_until(qr/go/,
		"\\echo go\nUPDATE $tab SET $col = $col + 10 WHERE $key = 1;\n");
	wait_for_writer_wait();
	$s1->query_safe('COMMIT');
	$s2->query_safe('SELECT 1');
	is($node->safe_psql('postgres', "SELECT $col FROM $tab WHERE $key = 1"),
		'11', "READ COMMITTED applies a waiting update to the new version ($tab)");

	# A row deleted meanwhile is skipped.
	$s1->query_safe("BEGIN; DELETE FROM $tab WHERE $key = 2;");
	$s2->query_until(qr/go/,
		"\\echo go\nUPDATE $tab SET $col = 99 WHERE $key = 2;\n");
	wait_for_writer_wait();
	$s1->query_safe('COMMIT');
	$s2->query_safe('SELECT 1');
	is($node->safe_psql('postgres', "SELECT count(*) FROM $tab WHERE $key = 2"),
		'0', "a waiting update skips a row deleted meanwhile ($tab)");
}

# A waiting conditional update re-checks the condition on the new version:
# a row updated meanwhile to no longer match is left alone (and the update
# isn't applied to the old version, losing the other one).
$node->safe_psql('postgres', q{
	CREATE TABLE rc (id int PRIMARY KEY, v int);
	INSERT INTO rc VALUES (1, 10), (2, 20);
});
$s1->query_safe('BEGIN; UPDATE rc SET v = 100 WHERE id = 2;');
$s2->query_until(qr/go/,
	"\\echo go\nUPDATE rc SET v = v + 1 WHERE v < 50;\n");
wait_for_writer_wait();
$s1->query_safe('COMMIT');
$s2->query_safe('SELECT 1');
is($node->safe_psql('postgres', 'SELECT array_agg(v ORDER BY id) FROM rc'),
	'{11,100}', 'a waiting update re-checks its condition on the new version');

# An insert that waits for another of the same key fails once it commits,
# and the committed row stays; an upsert updates it instead.
$s1->query_safe(q{BEGIN; INSERT INTO rc VALUES (3, 30);});
$s2->query_until(qr/go/, "\\echo go\nINSERT INTO rc VALUES (3, 31);\n");
wait_for_writer_wait();
$s1->query_safe('COMMIT');
$s2->query('SELECT 1');
like($s2->{stderr}, qr/duplicate key value violates unique constraint "rc_pkey"/,
	'an insert that waited for the same key fails');
$s2->{stderr} = '';
$s1->query_safe(q{BEGIN; INSERT INTO rc VALUES (4, 40);});
$s2->query_until(qr/go/, "\\echo go\nINSERT INTO rc VALUES (4, 41) "
	. "ON CONFLICT (id) DO UPDATE SET v = rc.v + 1;\n");
wait_for_writer_wait();
$s1->query_safe('COMMIT');
$s2->query_safe('SELECT 1');
is($node->safe_psql('postgres', 'SELECT array_agg(v ORDER BY id) FROM rc'),
	'{11,100,30,41}', 'the committed rows stay; the upsert updates the one it waited for');

# REPEATABLE READ refuses to update a row changed since its snapshot.
$s1->query_safe('BEGIN; UPDATE t SET v = v + 1 WHERE id = 1;');
$s2->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1;');
$s2->query_until(qr/go/, "\\echo go\nUPDATE t SET v = 0 WHERE id = 1;\n");
wait_for_writer_wait();
$s1->query_safe('COMMIT');
$s2->query('SELECT 1');
like($s2->{stderr}, qr/could not serialize access due to concurrent update/,
	'REPEATABLE READ fails on a row updated meanwhile');
$s2->{stderr} = '';
$s2->query_safe('ROLLBACK');

$s1->quit;
$s2->quit;
done_testing();
