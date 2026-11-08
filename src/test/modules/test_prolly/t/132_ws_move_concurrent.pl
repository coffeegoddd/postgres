# Copyright (c) 2026, PostgreSQL Global Development Group

# A move in place beside others' sessions, and through a crash.
#
# While a move of rows alone is open, a reader sees the rows as they were and waits for nothing, a
# snapshot taken before keeps seeing them so, and a writer waits for it and then writes on the
# target.  A move waits for a writer open before it, and moves from what that writer leaves; a
# second move waits for the first, and moves from what it leaves.  Another branch's writers wait
# for nothing; DDL anywhere in the database waits, as capture's commit of what DDL changes does.
# A move that alters a table has its readers wait, as an ALTER's do; readers of another table don't.
# Readers of a sequence of the schema wait for any move, which holds them as it may set them.
#
# A crash with a move open, or with its barrier taken, leaves the rows as they were, and capture
# with them; a crash once it commits, before capture applies it, leaves the target, which capture
# reaches, and lets go of what it held for it.

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
my $injections = $ENV{enable_injection_points} eq 'yes' && !$windows_os;
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points') if $injections;

my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $other = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";
my $on_other = "SET catalog_branch = $other;\n";

# (each with a time to answer in, so that one that waits fails, and doesn't hang)
sub sql
{
	my ($sql) = @_;
	return $node->safe_psql('postgres', "$on$sql", timeout => $PostgreSQL::Test::Utils::timeout_default);
}

sub quickly
{
	my ($sql, $prefix) = @_;
	my $waited;
	my ($rc, $out, $err) = $node->psql('postgres', ($prefix // $on) . $sql, timeout => 10, timed_out => \$waited,
		on_error_stop => 1);
	return 'waited' if $waited;
	return defined $rc && $rc == 0 ? $out : "failed: $err";
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
my $told = q{SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'capture/told/%'};

sub session
{
	my ($prefix) = @_;
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->query_safe($prefix // $on);
	return ($s, $s->query_safe('SELECT pg_backend_pid()'));
}

# Whether a session waits on another, and only on it.
sub waits_on
{
	my ($waiter, $holder) = @_;
	return $node->poll_query_until('postgres', qq{SELECT pg_blocking_pids($waiter) = ARRAY[$holder]});
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql(q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner;
	SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, v integer); INSERT INTO app.t VALUES (1, 1), (2, 1), (3, 1);
	CREATE TABLE app.u (id integer PRIMARY KEY); INSERT INTO app.u VALUES (1);
	CREATE SEQUENCE app.s});
my $first = commit_root();
sql(q{SET ROLE app_owner; UPDATE app.t SET v = 2; INSERT INTO app.t VALUES (4, 2)});
my $second = commit_root();
sql(q{SET ROLE app_owner; ALTER TABLE app.t ADD COLUMN w text});
my $third = commit_root();
sql(q{SET ROLE app_owner; ALTER TABLE app.t DROP COLUMN w});
is(read_root(), $second, 'the schema is at the second');
$node->safe_psql('postgres', "${on_other}CREATE SCHEMA other VERSIONED; CREATE TABLE other.o (id integer PRIMARY KEY)");

# A move of rows alone to the first, held open.
sql(qq{SELECT test_ws_move_prepare('app', '$first')});
my ($before) = session();
$before->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM app.t});
my ($mover, $mover_pid) = session();
$mover->query_safe(qq{BEGIN; SELECT test_ws_move('app', '$first')});
is(quickly(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t}), '1:2,2:2,3:2,4:2',
	'while it is open, a reader sees the rows as they were, and waits for nothing');
is(quickly(q{INSERT INTO other.o VALUES (1) RETURNING id}, $on_other), '1', 'another branch\'s writer waits for nothing');
my ($writer, $writer_pid) = session();
$writer->query_until(qr/started/, "\\echo started\nUPDATE app.t SET v = v + 10 WHERE id IN (1, 4) RETURNING id;\n\\echo written\n");
ok(waits_on($writer_pid, $mover_pid), 'a writer waits for it');
my ($ddl, $ddl_pid) = session($on_other);
$ddl->query_until(qr/started/, "\\echo started\nCREATE TABLE other.o2 (id integer);\n\\echo made\n");
ok(waits_on($ddl_pid, $mover_pid), 'DDL on another branch waits for it too');
$mover->query_safe('COMMIT');
is($writer->query_until(qr/written/, ''), "1\nwritten\n", 'once it commits, the writer writes on the target: a row it lacks is not there');
$ddl->query_until(qr/made/, '');
is(rows(), '1:11,2:1,3:1', 'and one it has is updated from what the target has');
is($before->query_safe(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t}), '1:2,2:2,3:2,4:2',
	'a snapshot taken before still sees them as they were');
$before->query_safe('COMMIT');
my $now = read_root();
is(capture_root(), $now, 'capture has what the tables hold');
is($node->safe_psql('postgres', $told), '0', 'and holds nothing for the move');
$_->quit for ($before, $mover, $writer, $ddl);

# A move waits for a writer open before it, and moves from what that writer leaves.
($writer, $writer_pid) = session();
$writer->query_safe(q{BEGIN; INSERT INTO app.t VALUES (20, 20)});
sql(qq{SELECT test_ws_move_prepare('app', '$second')});
($mover, $mover_pid) = session();
$mover->query_until(qr/started/, "\\echo started\nSELECT test_ws_move('app', '$second') IS NOT NULL;\n\\echo moved\n");
ok(waits_on($mover_pid, $writer_pid), 'a move waits for a writer open before it');
$writer->query_safe('COMMIT');
$mover->query_until(qr/moved/, '');
is(rows(), '1:2,2:2,3:2,4:2', 'and moves from what it left: its row is not the target\'s');
is(read_root(), $second, 'the tables hold the target');
$_->quit for ($writer, $mover);

# A second move waits for the first, and moves from what it leaves.
sql(qq{SELECT test_ws_move_prepare('app', '$first')});
($mover, $mover_pid) = session();
$mover->query_safe(qq{BEGIN; SELECT test_ws_move('app', '$first')});
my ($mover2, $mover2_pid) = session();
$mover2->query_until(qr/started/, "\\echo started\nSELECT test_ws_move('app', '$second') IS NOT NULL;\n\\echo moved\n");
ok(waits_on($mover2_pid, $mover_pid), 'a second move waits for the first');
$mover->query_safe('COMMIT');
$mover2->query_until(qr/moved/, '');
is(read_root(), $second, 'and moves from what it left');
is(capture_root(), $second, 'and capture has it');
$_->quit for ($mover, $mover2);

# A move that alters a table, held open: its readers wait, as a sequence's do; another's don't.
sql(qq{SELECT test_ws_move_prepare('app', '$third')});
($mover, $mover_pid) = session();
$mover->query_safe(qq{BEGIN; SELECT test_ws_move('app', '$third')});
is(quickly(q{SELECT count(*) FROM app.u}), '1', 'a reader of a table it doesn\'t alter waits for nothing');
my ($reader, $reader_pid) = session();
$reader->query_until(qr/started/, "\\echo started\nSELECT count(*) FROM app.t;\n\\echo read\n");
ok(waits_on($reader_pid, $mover_pid), 'a reader of the table it alters waits for it');
my ($seq, $seq_pid) = session();
$seq->query_until(qr/started/, "\\echo started\nSELECT last_value FROM app.s;\n\\echo read\n");
ok(waits_on($seq_pid, $mover_pid), 'and a reader of a sequence of the schema');
$mover->query_safe('COMMIT');
like($reader->query_until(qr/read/, ''), qr/^4\n/m, 'and then reads the target');
$seq->query_until(qr/read/, '');
is(sql(q{SELECT count(*) FROM pg_attribute WHERE attrelid = 'app.t'::regclass AND attname = 'w'}), '1',
	'which has the column added');
is(capture_root(), $third, 'capture has it');
$_->quit for ($mover, $reader, $seq);
sql(qq{SELECT test_ws_move('app', '$second')});
$now = read_root();

# A crash with a move open: the rows as they were, and capture with them.
sql(qq{SELECT test_ws_move_prepare('app', '$first')});
($mover) = session();
$mover->query_safe(qq{BEGIN; SELECT test_ws_move('app', '$first')});
$node->stop('immediate');
eval { $mover->quit };
$node->start;
is(rows(), '1:2,2:2,3:2,4:2', 'after a crash with a move open, the rows are as they were');
is(read_root(), $now, 'and the tables hold the root they did');
is(capture_root(), $now, 'and capture has it');

SKIP:
{
	skip 'injection points not supported by this build', 9 unless $injections;

	# A crash with a move's barrier taken, before it writes anything.
	sql(qq{SELECT test_ws_move_prepare('app', '$first')});
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-barrier-saved', 'wait')});
	($mover) = session();
	$mover->query_until(qr/started/, "\\echo started\nSELECT test_ws_move('app', '$first');\n");
	$node->wait_for_event('client backend', 'capture-barrier-saved');
	$node->stop('immediate');
	eval { $mover->quit };
	$node->start;
	is(rows(), '1:2,2:2,3:2,4:2', 'after a crash with a move\'s barrier taken, the rows are as they were');
	is(read_root(), $now, 'and the tables hold the root they did');
	is(capture_root(), $now, 'and capture has it');

	# A crash once a move commits, with capture held from applying it.
	sql(qq{SELECT test_ws_move_prepare('app', '$first')});
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-apply-begin', 'wait')});
	# (the move's own barrier has nothing to apply, and the worker then waits to apply the move)
	sql(qq{SELECT test_ws_move('app', '$first')});
	$node->wait_for_event('capture worker', 'capture-apply-begin');
	ok($node->safe_psql('postgres', $told) > 0, 'once a move commits, what it told capture is held until applied');
	$node->stop('immediate');
	$node->start;
	is(rows(), '1:1,2:1,3:1', 'after a crash before capture applied it, the rows are the target');
	is(read_root(), $first, 'and the tables hold it');
	is(capture_root(), $first, 'and capture reaches it');
	is($node->safe_psql('postgres', $told), '0', 'and lets go of what it held for it');
	sql(qq{SELECT test_ws_move('app', '$second')});
	is(read_root(), $second, 'a move after them is as any');
}

done_testing();
