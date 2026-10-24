# Copyright (c) 2026, PostgreSQL Global Development Group

# AnyLockHasWaiters(), which a paced fold asks before each nap so that it
# gives way: true while another backend waits for a lock this one holds,
# a fast-path one among them (found in the shared table once the strong
# locker moved it there), or for this one's virtual transaction (as
# CREATE INDEX CONCURRENTLY waits out older snapshots).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('lock_has_waiters');
$node->init;
$node->append_conf('postgresql.conf', qq{autovacuum = off});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE plain (id int);
});

my $a = $node->background_psql('postgres');
my $b = $node->background_psql('postgres');
my $apid = $a->query_safe(q{SELECT pg_backend_pid()});

# A weak lock taken by the fast path.
$a->query_safe(q{BEGIN});
$a->query_safe(q{SELECT count(*) FROM plain});
is( $node->safe_psql(
		'postgres',
		qq{SELECT fastpath FROM pg_locks WHERE pid = $apid
		   AND relation = 'plain'::regclass}),
	't',
	'the lock is a fast-path one');
is($a->query_safe(q{SELECT test_any_lock_has_waiters()}),
	'f', 'nobody waits for it');

# A strong locker waits for it.
$b->query_until(qr/locking/,
	"\\echo locking\nBEGIN;\nLOCK TABLE plain IN ACCESS EXCLUSIVE MODE;\nCOMMIT;\n");
ok( $node->poll_query_until(
		'postgres',
		q{SELECT count(*) > 0 FROM pg_locks
		  WHERE relation = 'plain'::regclass AND NOT granted}),
	'a strong locker waits');
is($a->query_safe(q{SELECT test_any_lock_has_waiters()}),
	't', 'one waits for a fast-path lock held');
$a->query_safe(q{COMMIT});
$b->query_safe(q{SELECT 1});

# A snapshot an index build waits out: its virtual transaction's lock.
$a->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ});
$a->query_safe(q{SELECT 1});
is($a->query_safe(q{SELECT test_any_lock_has_waiters()}),
	'f', 'nobody waits for the snapshot');
$b->query_until(qr/building/,
	"\\echo building\nCREATE INDEX CONCURRENTLY plain_id ON plain (id);\n");
ok( $node->poll_query_until(
		'postgres',
		q{SELECT count(*) > 0 FROM pg_locks
		  WHERE locktype = 'virtualxid' AND NOT granted}),
	'the index build waits for the snapshot');
is($a->query_safe(q{SELECT test_any_lock_has_waiters()}),
	't', 'one waits for the virtual transaction');
$a->query_safe(q{COMMIT});
$b->query_safe(q{SELECT 1});
is( $node->safe_psql(
		'postgres', q{SELECT count(*) FROM pg_index
		WHERE indexrelid = 'plain_id'::regclass AND indisvalid}),
	'1',
	'and the index is built');

$a->quit;
$b->quit;
$node->stop;
done_testing();
