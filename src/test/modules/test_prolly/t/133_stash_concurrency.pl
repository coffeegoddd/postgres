# Copyright (c) 2026, PostgreSQL Global Development Group

# A stash popped or applied is chosen with the refs locked: another
# session's drop or push, committed while the pop waits for the lock, is
# seen before the pop chooses, so it never applies a stash dropped since,
# nor drops one it didn't apply.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('stash_concurrency');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA st VERSIONED;
	CREATE TABLE st.t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'empty');
});

# s: row 1's stash, then row 2's (the newest, stash@{0})
sub two_stashes
{
	sql(q{
		SELECT dolt_reset('--hard');
		SELECT dolt_stash('clear', 's');
		INSERT INTO st.t VALUES (1);
		SELECT dolt_stash('push', 's');
		INSERT INTO st.t VALUES (2);
		SELECT dolt_stash('push', 's');
	});
}

sub wait_for_lock
{
	return $node->poll_query_until('postgres', q{
		SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event_type = 'Lock' AND query LIKE '%dolt_stash(''pop''%'});
}

my $other = $node->background_psql('postgres');
my $popper = $node->background_psql('postgres');

# A drop of the newest, committed while a pop waits: the pop takes the
# one left, row 1's, and none is lost.
two_stashes();
$other->query_safe(q{BEGIN; SELECT dolt_stash('drop', 's')});
$popper->query_until(qr/started/, q{
	\echo started
	SELECT dolt_stash('pop', 's');
});
ok(wait_for_lock(), 'a pop waits for a drop in progress');
$other->query_safe('COMMIT');
$popper->query_safe('SELECT 1');
is($popper->query_safe(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM st.t}),
	'1', 'the pop applies the stash left, not the one dropped');
is(sql(q{SELECT count(*) FROM dolt_stashes}), '0', 'and drops that one');

# A push, committed while a pop waits: the pop takes the new newest.
$popper->query_safe(q{SELECT dolt_reset('--hard')});
two_stashes();
$other->query_safe(q{BEGIN; INSERT INTO st.t VALUES (3); SELECT dolt_stash('push', 's')});
$popper->query_until(qr/started/, q{
	\echo started
	SELECT dolt_stash('pop', 's');
});
ok(wait_for_lock(), 'a pop waits for a push in progress');
$other->query_safe('COMMIT');
$popper->query_safe('SELECT 1');
is($popper->query_safe(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM st.t}),
	'3', 'the pop applies the stash pushed');
is(sql(q{SELECT string_agg(stash_id, ',' ORDER BY stash_id) FROM dolt_stashes}),
	'stash@{0},stash@{1}', 'and the two before it are kept');

$other->quit;
$popper->quit;
$node->stop;
done_testing();
