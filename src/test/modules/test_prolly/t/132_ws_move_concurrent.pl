# Copyright (c) 2026, PostgreSQL Global Development Group

# A move in place beside others' sessions, and through a crash.  While a move is open, a reader sees
# the rows as they were and waits for nothing, a writer waits, and a snapshot taken before keeps
# seeing them as they were; once it commits, readers see the target and the writer's row goes in
# on it.  A crash with a move open leaves the rows as they were, and capture with them; a crash once
# it commits leaves the target, and capture reaches it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move_concurrent');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";

sub sql
{
	my ($sql) = @_;
	return $node->safe_psql('postgres', "$on$sql");
}

my $commits = 0;
sub commit_root
{
	$commits++;
	return sql(qq{SELECT test_capture_commit_on('app', 'main', 'c$commits')});
}

sub read_root { return sql(q{SELECT test_ws_root_capture('app')}); }
sub capture_root { return sql(q{SELECT test_capture_barrier('app')}); }
sub rows { return sql(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t}); }

# Whether a session's statement waits on a lock.
sub waiting
{
	my ($like) = @_;
	return $node->poll_query_until('postgres',
		qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE '$like'});
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql(q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner;
	SET ROLE app_owner; CREATE TABLE app.t (id integer PRIMARY KEY, v integer); INSERT INTO app.t VALUES (1, 1), (2, 1), (3, 1)});
my $first = commit_root();
sql(q{SET ROLE app_owner; UPDATE app.t SET v = 2; INSERT INTO app.t VALUES (4, 2)});
my $second = commit_root();
is(rows(), '1:2,2:2,3:2,4:2', 'the schema is at the second');

# A move to the first, held open.
sql(qq{SELECT test_ws_move_prepare('app', '$first')});
my $before = $node->background_psql('postgres');
$before->query_safe(qq{$on BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM app.t});
my $mover = $node->background_psql('postgres');
$mover->query_safe(qq{$on BEGIN; SELECT test_ws_move('app', '$first')});
is(rows(), '1:2,2:2,3:2,4:2', 'while it is open, a reader sees the rows as they were, and waits for nothing');
my $writer = $node->background_psql('postgres');
$writer->query_until(qr/started/, "\\echo started\n$on INSERT INTO app.t VALUES (10, 10);\n\\echo written\n");
ok(waiting('INSERT INTO app.t%'), 'a writer waits');
$mover->query_safe('COMMIT');
$writer->query_until(qr/written/, '');
is(rows(), '1:1,2:1,3:1,10:10', 'once it commits, the rows are the target, and the writer\'s row goes in on it');
is($before->query_safe('SELECT string_agg(id || \':\' || v, \',\' ORDER BY id) FROM app.t'), '1:2,2:2,3:2,4:2',
	'a snapshot taken before still sees them as they were');
$before->query_safe('COMMIT');
my $now = read_root();
is(capture_root(), $now, 'capture has what the tables hold');
$_->quit for ($before, $mover, $writer);

# A crash with a move open: the rows as they were, and capture with them.
sql(qq{SELECT test_ws_move_prepare('app', '$second')});
$mover = $node->background_psql('postgres');
$mover->query_safe(qq{$on BEGIN; SELECT test_ws_move('app', '$second')});
$node->stop('immediate');
eval { $mover->quit };
$node->start;
is(rows(), '1:1,2:1,3:1,10:10', 'after a crash with a move open, the rows are as they were');
is(read_root(), $now, 'and the tables hold the root they did');
is(capture_root(), $now, 'and capture has it');

# A crash once a move commits: the target, which capture reaches.
sql(qq{SELECT test_ws_move_prepare('app', '$second')});
sql(qq{SELECT test_ws_move('app', '$second')});
$node->stop('immediate');
$node->start;
is(rows(), '1:2,2:2,3:2,4:2', 'after a crash once a move commits, the rows are the target');
is(read_root(), $second, 'and the tables hold it');
is(capture_root(), $second, 'and capture reaches it');

# And a move after both.
sql(qq{SELECT test_ws_move_prepare('app', '$first')});
sql(qq{SELECT test_ws_move('app', '$first')});
is(read_root(), $first, 'a move after them: the tables hold the root');
is(capture_root(), $first, 'and capture has it');

done_testing();
