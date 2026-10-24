# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's first use made of the pages kept as of its head: they are as
# the head's commit left them, so rows committed on the source after a
# repeatable read snapshot are never in them, and the first use needn't
# wait for the source's writers nor be refused for them.  But pages kept
# as of a commit made after the snapshot hold rows the snapshot doesn't
# see: that first use is refused, as one copying the source would be, and
# the retry, by a new snapshot, has the head's rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('first_use_kept_snapshots');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text);
	ALTER TABLE app.t ALTER pad SET STORAGE EXTERNAL;
	INSERT INTO app.t VALUES (1, 1, repeat('x', 5000)), (2, 2, NULL);
	SELECT dolt_commit('-Am', 'c1');
	SELECT dolt_branch('k1');
	SELECT dolt_branch('k2');
	SELECT dolt_branch('k3');
});

my $writer = $node->background_psql('postgres');
my $user = $node->background_psql('postgres', on_error_stop => 0);
my $rows = q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t};
sub kept_tables
{
	sql('SELECT pg_stat_force_next_flush()');
	return sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'first_use_tables_kept'});
}

# a writer of main commits after the snapshot: the kept pages hold none of
# it; the first use isn't refused, and the branch is its head
my $before = kept_tables();
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(q{INSERT INTO app.t VALUES (4, 4); DELETE FROM app.t WHERE id = 2});
$user->query_safe(q{SELECT pg_stat_force_next_flush(); SELECT dolt_checkout('k1')});
is($user->query_safe($rows), '1:1,2:2', 'a repeatable read first use of kept pages: the head\'s rows');
is($user->query_safe(q{SELECT length(pad) FROM app.t WHERE id = 1}), '5000',
	'and its values out of line');
$user->query_safe('COMMIT');
is($user->query_safe($rows), '1:1,2:2', 'and after the commit');
cmp_ok(kept_tables(), '>', $before, 'made of kept pages');
$user->query_safe(q{SELECT dolt_checkout('main')});

# a writer in flight: the first use doesn't wait for it
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(q{BEGIN; UPDATE app.t SET v = 7 WHERE id = 1});
$user->query_safe(q{SET lock_timeout = '1s'; SELECT dolt_checkout('k2')});
is($user->query_safe($rows), '1:1,2:2', 'a writer in flight on main: not waited for');
$writer->query_safe('COMMIT');
$user->query_safe('COMMIT');
$user->query_safe(q{RESET lock_timeout; SELECT dolt_checkout('main')});

# a commit made after the snapshot, kept: its pages hold rows the snapshot
# doesn't see; refused, and retried
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$writer->query_safe(q{UPDATE app.t SET v = 8 WHERE id = 1;
	SELECT dolt_commit('-am', 'c2'); SELECT dolt_branch('k4')});
$user->query(q{SELECT dolt_checkout('k4')});
like($user->{stderr}, qr/could not serialize access/,
	'pages kept as of a commit after the snapshot: refused');
$user->{stderr} = '';
$user->query_safe('ROLLBACK');
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$user->query_safe(q{SELECT dolt_checkout('k4')});
is($user->query_safe($rows), '1:8,4:4', 'retried, the head\'s rows');
$user->query_safe('COMMIT');
$user->query_safe(q{SELECT dolt_checkout('main')});

$writer->quit;
$user->quit;
$node->stop;
done_testing();
