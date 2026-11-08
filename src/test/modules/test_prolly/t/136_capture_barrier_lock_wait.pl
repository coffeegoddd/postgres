# Copyright (c) 2026, PostgreSQL Global Development Group

# A barrier waits on another transaction for no longer than half of deadlock_timeout as it builds
# its root, so that a deadlock with it fails it and not the other; past that, it's a serialization
# failure, to be tried again, and not a lock timeout.  So for a barrier, and a move's, that waits
# on another's open transaction that has put the same chunks: one with a barrier taken in it, as
# one taken just before another's commits.  Once that's ended, each has its root.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('barrier_lock_wait');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf',
	"autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\ndeadlock_timeout = 200ms\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";

sub sql
{
	my ($sql) = @_;
	return $node->safe_psql('postgres', "$on$sql");
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql(q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, v integer); INSERT INTO app.t VALUES (1, 1), (2, 1)});
my $first = sql(q{SELECT test_capture_commit_on('app', 'main', 'c1')});

# A barrier taken in a transaction left open, of tables changed since: what it put is in progress.
sub held_barrier
{
	my ($v) = @_;
	sql(qq{SET ROLE app_owner; UPDATE app.t SET v = $v});
	my $holder = $node->background_psql('postgres', on_error_stop => 0);
	$holder->query_safe($on);
	$holder->query_safe(q{BEGIN; SELECT test_capture_barrier('app')});
	return $holder;
}

sub tried
{
	my ($what, $sql) = @_;
	my ($rc, $out, $err) = $node->psql('postgres', "$on$sql");
	isnt($rc, 0, "$what, while another's transaction that put the same chunks is open, fails");
	like($err, qr/ERROR:  could not serialize a commit from capture: it waited on another transaction/,
		"$what: as a serialization failure");
	unlike($err, qr/lock timeout/, "$what: not a lock timeout");
	like($err, qr/CONTEXT:  .*waiting for ShareLock on transaction \d+/, "$what: naming what it waited on");
}

my $holder = held_barrier(2);
tried('a barrier', q{SELECT test_capture_barrier('app')});
$holder->query_safe('COMMIT');
$holder->quit;
my $now = sql(q{SELECT test_capture_barrier('app')});
is($now, sql(q{SELECT test_ws_root_capture('app')}), 'once it has ended, the barrier has its root');

$holder = held_barrier(3);
sql(qq{SELECT test_ws_move_prepare('app', '$first')});
tried('a move', qq{SELECT test_ws_move('app', '$first')});
$holder->query_safe('COMMIT');
$holder->quit;
sql(qq{SELECT test_ws_move('app', '$first')});
is(sql(q{SELECT test_ws_root_capture('app')}), $first, 'once it has ended, the move moves');
is(sql(q{SELECT test_capture_barrier('app')}), $first, 'and capture has it');

done_testing();
