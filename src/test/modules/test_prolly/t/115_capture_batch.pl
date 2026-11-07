# Copyright (c) 2026, PostgreSQL Global Development Group

# A worker far behind applies what it's read as it goes, rather than hold it all, and a map is written
# a bounded batch at a time: nothing's lost or counted twice for that, with the worker killed or otherwise.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('batch');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf',
	"log_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\nlogical_decoding_work_mem = 64kB\nlog_min_messages = debug1\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch;";
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED");
$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_history.capture_positions WHERE branch = 0}) or die 'no worker';
my $slot = 'prolly_capture_' . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = current_database()});
my $worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});

sub run
{
	return $node->safe_psql('postgres', "$on SET search_path = s, public; $_[0]");
}

sub caught_up
{
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die 'capture did not catch up';
}

sub same_map
{
	my ($table, $what) = @_;
	caught_up();
	is(run(qq{SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.$table'::regclass::oid}),
		run(qq{SELECT test_ws_rows_hash('s', '$table')}), $what);
}

# Many transactions of about a kilobyte each, on a table of each kind.
sub write_many
{
	my ($n, $after_first) = @_;
	my @script;
	for my $i (1 .. $n)
	{
		push @script, "INSERT INTO keyed VALUES ($i + (SELECT coalesce(max(id), 0) FROM keyed), repeat('k', 800));",
		  "INSERT INTO nokey VALUES ($i % 7, repeat('n', 800));";
		push @script, $after_first if $i == 1 && defined $after_first;
	}
	return run(join("\n", @script));
}

run(q{CREATE TABLE keyed (id integer PRIMARY KEY, v text); CREATE TABLE nokey (a integer, v text)});
caught_up();

# applied as it goes: more than once in one read, each time confirmed
SKIP:
{
	skip 'no way to stop a process', 4 if $windows_os;
	my $at = -s $node->logfile;
	kill 'STOP', $worker;
	write_many(300);
	kill 'CONT', $worker;
	same_map('keyed', 'many transactions, with a key');
	same_map('nokey', 'and with none');
	my @applied = slurp_file($node->logfile, $at) =~ /capture tree: applied (\d+) changes/g;
	cmp_ok(scalar(@applied), '>', 2, 'applied in several goes');
	cmp_ok((sort { $b <=> $a } @applied)[0], '<', 500, 'none of them everything');
}

# killed between them: told again from what's confirmed, and passed over what isn't
SKIP:
{
	skip 'injection points not supported by this build', 5 if $ENV{enable_injection_points} ne 'yes' || $windows_os;
	$node->safe_psql('postgres', "CREATE EXTENSION injection_points; SELECT injection_points_attach('capture-applied', 'wait')");
	caught_up();
	kill 'STOP', $worker;
	write_many(300);
	kill 'CONT', $worker;
	$node->wait_for_event('capture worker', 'capture-applied');

	# (the slot's told of each batch, so the WAL behind it can go while the worker's still far behind)
	my $was_at = $node->safe_psql('postgres', qq{SELECT confirmed_flush_lsn FROM pg_replication_slots WHERE slot_name = '$slot'});
	$node->safe_psql('postgres', q{SELECT injection_points_wakeup('capture-applied')});
	ok( $node->poll_query_until('postgres',
			qq{SELECT confirmed_flush_lsn > '$was_at'::pg_lsn AND confirmed_flush_lsn < pg_current_wal_flush_lsn() FROM pg_replication_slots WHERE slot_name = '$slot'}),
		'the slot moves on after a batch, before the end');
	$node->wait_for_event('capture worker', 'capture-applied');
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applied')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($worker)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $worker})
	  or die 'no worker came back';
	$worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	same_map('keyed', 'killed between batches, with a key');
	same_map('nokey', 'and with none');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');

	# a transaction open across what's confirmed is still told, in full, after the worker restarts
	caught_up();
	my $open = $node->background_psql('postgres');
	$open->query_safe("$on BEGIN; INSERT INTO s.keyed VALUES (800001, 'open'); INSERT INTO s.nokey VALUES (800001, 'open')");
	my $before_open = $node->safe_psql('postgres', qq{SELECT confirmed_flush_lsn FROM pg_replication_slots WHERE slot_name = '$slot'});
	write_many(100);
	$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn > '$before_open'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'}) or die 'nothing confirmed';
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($worker)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $worker})
	  or die 'no worker came back';
	$worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	$open->query_safe('COMMIT');
	$open->quit;
	same_map('keyed', 'a transaction open across what was confirmed');
	same_map('nokey', 'with no key too');
}

# killed in the middle of committing what it applied: none of it is, and it's applied again whole
SKIP:
{
	skip 'injection points not supported by this build', 5 if $ENV{enable_injection_points} ne 'yes' || $windows_os;
	$node->safe_psql('postgres', "CREATE EXTENSION injection_points; SELECT injection_points_attach('capture-applying', 'wait')");
	caught_up();
	my $state = qq{SELECT (SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.keyed'::regclass::oid)
		|| ' ' || (SELECT string_agg(branch || ':' || lsn, ',' ORDER BY branch) FROM pg_history.capture_positions)};
	my $was = $node->safe_psql('postgres', "$on $state");
	# (what's flushed after the first transaction's commit: the first apply with anything in it waits,
	# so every pass before it read less than that commit, and the slot is before it)
	my $first = write_many(50, 'SELECT pg_current_wal_insert_lsn();');
	$node->wait_for_event('capture worker', 'capture-applying');
	is($node->safe_psql('postgres', "$on $state"), $was, 'what it applied, map and positions, is not committed yet');
	is($node->safe_psql('postgres', qq{SELECT confirmed_flush_lsn < '$first'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'}),
		't', 'nor the slot moved past the first of what it applied');
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applying'); SELECT injection_points_wakeup('capture-applying')});
	caught_up();
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-applying', 'wait')});
	# and applied at the end of what it read, rather than as a batch past logical_decoding_work_mem
	$first = run(q{INSERT INTO keyed VALUES (-1, 'small'); SELECT pg_current_wal_insert_lsn()});
	$node->wait_for_event('capture worker', 'capture-applying');
	is($node->safe_psql('postgres', qq{SELECT confirmed_flush_lsn < '$first'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'}),
		't', 'nor for a small write, applied at the end of what was read');
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applying')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($worker)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $worker})
	  or die 'no worker came back';
	$worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	same_map('keyed', 'killed before its commit, with a key');
	same_map('nokey', 'and with none');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
}

# one transaction, more than a batch of a table's map, with rows the same over the edge of one
run(q{BEGIN; INSERT INTO keyed SELECT g + 100000, 'one transaction' FROM generate_series(1, 3500) g; UPDATE keyed SET v = 'again' WHERE id > 100000; COMMIT});
same_map('keyed', 'one transaction, many batches, with a key');
run(q{BEGIN; INSERT INTO nokey SELECT 1000, 'same' FROM generate_series(1, 2500); DELETE FROM nokey WHERE a = 1000 AND ctid IN (SELECT ctid FROM nokey WHERE a = 1000 LIMIT 700); COMMIT});
same_map('nokey', 'and with none, the same row over and over');

# a deferrable key's transaction is written whole, whatever its size, as a batch's edge could be with
# keys on two rows
run(q{CREATE TABLE deferred (id integer PRIMARY KEY DEFERRABLE, v text); INSERT INTO deferred SELECT g, 'v' || g FROM generate_series(1, 2500) g});
same_map('deferred', 'a deferrable key, more than a batch');
run(q{UPDATE deferred SET id = id + 1});
same_map('deferred', 'keys moved onto each other, more than a batch');

# what's in a batch already written is in the map, for a value an update left out of line
run(q{CREATE TABLE out_of_line (id integer PRIMARY KEY, n integer, v text); ALTER TABLE out_of_line ALTER v SET STORAGE EXTERNAL;
	INSERT INTO out_of_line VALUES (1, 0, repeat('old', 5000))});
caught_up();
run(q{BEGIN; UPDATE out_of_line SET v = repeat('new', 5000) WHERE id = 1; INSERT INTO out_of_line SELECT g + 10, 0, 'x' FROM generate_series(1, 2500) g; UPDATE out_of_line SET n = 1 WHERE id = 1; COMMIT});
same_map('out_of_line', 'changed, a batch written, and left');
run(q{BEGIN; INSERT INTO out_of_line VALUES (2, 0, repeat('two', 5000)); UPDATE out_of_line SET n = 1 WHERE id = 2; UPDATE out_of_line SET id = 3 WHERE id = 2; COMMIT});
same_map('out_of_line', 'and in the same batch');

# a batch of bytes, not rows: cells of their own, as what's in a tree of its own is only its name here
my @columns = map { "c$_ text" } 1 .. 20;
my $cell = q{(SELECT string_agg(md5(g::text || s::text), '') FROM generate_series(1, 62) s)};
run('CREATE TABLE wide (id integer PRIMARY KEY, ' . join(', ', @columns) . ')');
caught_up();
my $wide_at = -s $node->logfile;
run('INSERT INTO wide SELECT g, ' . join(', ', ($cell) x 20) . ' FROM generate_series(1, 300) g');
same_map('wide', 'batches by size');
my $wide = $node->safe_psql('postgres', "$on SELECT 's.wide'::regclass::oid");
my @wrote = slurp_file($node->logfile, $wide_at) =~ /capture tree: wrote (\d+) edits of relation $wide/g;
cmp_ok(scalar(@wrote), '>', 1, 'in more than one');
cmp_ok((sort { $b <=> $a } @wrote)[0], '<', 300, 'none of them everything');

# a batch by edits has no more than it says
my $rows_at = -s $node->logfile;
run(q{CREATE TABLE narrow (id integer PRIMARY KEY); INSERT INTO narrow SELECT generate_series(1, 2500)});
same_map('narrow', 'batches by edits');
my $narrow = $node->safe_psql('postgres', "$on SELECT 's.narrow'::regclass::oid");
cmp_ok((sort { $b <=> $a } (slurp_file($node->logfile, $rows_at) =~ /capture tree: wrote (\d+) edits of relation $narrow/g))[0], '<=', 1000, 'of a thousand edits');

# a truncate is voided in its transaction, so what's held can still be applied for its size
SKIP:
{
	skip 'no way to stop a process', 2 if $windows_os;
	caught_up();
	my $at = -s $node->logfile;
	kill 'STOP', $worker;
	run('TRUNCATE narrow');
	write_many(200);
	kill 'CONT', $worker;
	same_map('keyed', 'written after a truncate');
	cmp_ok(scalar(() = slurp_file($node->logfile, $at) =~ /capture tree: applied \d+ changes/g), '>', 2, 'applied in several goes');
}

# a key made deferrable with writes on both sides is refused wherever a batch happens to end
{
	run(q{CREATE TABLE edge (id integer PRIMARY KEY, n integer)});
	caught_up();
	my $at = -s $node->logfile;
	$node->safe_psql('postgres', qq{$on BEGIN; INSERT INTO s.edge SELECT generate_series(1, 1000), 0;
		ALTER TABLE s.edge DROP CONSTRAINT edge_pkey; ALTER TABLE s.edge ADD PRIMARY KEY (id) DEFERRABLE; UPDATE s.edge SET n = 1 WHERE id = 1; COMMIT});
	ok($node->wait_for_log(qr/having its deferrable key changed/, $at), 'with the first side a batch exactly');
	run('VACUUM FULL edge');
	same_map('edge', 'which a rewrite gets past');

	# (it stopped the worker, and another's running now)
	$worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
}

# a barrier over what's more than a batch
SKIP:
{
	skip 'no way to stop a process', 1 if $windows_os;
	caught_up();
	kill 'STOP', $worker;
	write_many(200);
	my $bg = $node->background_psql('postgres');
	$bg->query_until(qr/started/, "\\echo started\n$on SELECT test_capture_barrier('s') AS root \\gset\n\\echo :root\n\\echo finished\n");
	$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'CaptureBarrier'}) or die 'no marker';
	kill 'CONT', $worker;
	$bg->{stdout} = '';
	$bg->{run}->pump until $bg->{stdout} =~ /finished/ || !$bg->{run}->pumpable;
	my ($root) = $bg->{stdout} =~ /^([0-9a-f]{40})$/m;
	$bg->quit;
	is($root, $node->safe_psql('postgres', "$on SELECT test_ws_root_capture('s')"), 'the same root as reading the tables');
}

# one transaction, which is told in one call, past what's held in memory
SKIP:
{
	skip 'injection points not supported by this build', 9 if $ENV{enable_injection_points} ne 'yes' || $windows_os;
	run(q{CREATE TABLE huge (id integer PRIMARY KEY, v text); CREATE TABLE huge_nokey (a integer, v text);
		CREATE TABLE huge_left (id integer PRIMARY KEY, n integer, v text); ALTER TABLE huge_left ALTER v SET STORAGE EXTERNAL;
		CREATE TABLE huge_deferred (id integer PRIMARY KEY DEFERRABLE, v text);
		INSERT INTO huge_left VALUES (1, 0, repeat('left', 5000));
		INSERT INTO huge_deferred SELECT g, repeat('d', 200) FROM generate_series(1, 1500) g});
	caught_up();
	my $at = -s $node->logfile;
	$node->safe_psql('postgres', qq{CREATE EXTENSION injection_points; SELECT injection_points_attach('capture-apply-begin', 'wait')});
	run(q{BEGIN;
		INSERT INTO huge SELECT g, repeat('h', 800) FROM generate_series(1, 20000) g;
		UPDATE huge SET v = 'changed' WHERE id % 3 = 0;
		DELETE FROM huge WHERE id % 5 = 0;
		INSERT INTO huge_nokey SELECT g % 10, repeat('n', 800) FROM generate_series(1, 20000) g;
		DELETE FROM huge_nokey WHERE a = 3;
		UPDATE huge_left SET v = repeat('newer', 5000);
		UPDATE huge_left SET n = 1;
		UPDATE huge_deferred SET id = id + 1;
		COMMIT});
	$node->wait_for_event('capture worker', 'capture-apply-begin');
	like(slurp_file($node->logfile, $at), qr/capture tree: spilled \d+ bytes/, 'one transaction goes to a file');

	# (what's in memory with all of it held, about to be applied: of tables each written past what's
	# held in memory, so that one's room being kept would show)
	my $memory_at = -s $node->logfile;
	my $pid = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	$node->safe_psql('postgres', "SELECT pg_log_backend_memory_contexts($pid)");
	ok($node->wait_for_log(qr/capture tree held: \d+ total/, $memory_at), 'what it holds in memory is logged');
	my ($held) = slurp_file($node->logfile, $memory_at) =~ /capture tree held: (\d+) total/;
	cmp_ok($held, '<', 256 * 1024, 'and is little, for one of 16 MB and more');
	$node->safe_psql('postgres', q{SELECT injection_points_wakeup('capture-apply-begin'); SELECT injection_points_detach('capture-apply-begin')});
	same_map('huge', 'with a key');
	same_map('huge_nokey', 'with none');
	same_map('huge_left', 'a value left out of line');
	same_map('huge_deferred', 'a deferrable key');

	# told whole, what's on file goes
	$at = -s $node->logfile;
	run(q{BEGIN; INSERT INTO huge SELECT g + 100000, repeat('h', 800) FROM generate_series(1, 5000) g; ALTER TABLE huge ALTER v TYPE varchar(1000); COMMIT});
	same_map('huge', 'on file, then told whole');
	run(q{UPDATE huge SET v = 'after' WHERE id = 1});
	same_map('huge', 'and written after');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
}

# what's wrong with a change isn't applied for its size, or a worker would never read as far as what
# gets past it
SKIP:
{
	skip 'no way to stop a process', 2 if $windows_os;
	run(q{CREATE TABLE stuck (id integer PRIMARY KEY); INSERT INTO stuck VALUES (1)});
	caught_up();
	my $at = -s $node->logfile;
	kill 'STOP', $worker;
	run(q{BEGIN; ALTER TABLE stuck DROP CONSTRAINT stuck_pkey; INSERT INTO stuck VALUES (2); ALTER TABLE stuck ADD PRIMARY KEY (id); COMMIT});
	write_many(200);
	run('VACUUM FULL stuck');
	kill 'CONT', $worker;
	same_map('stuck', 'a change that stops capture, a batch of others, and what gets past it');
	same_map('keyed', 'and the others');
}

is($node->safe_psql('postgres', q{SELECT ok FROM test_cs_check('pages')}), 't', 'and the store has all it should');
unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal/m, 'nothing crashed');
$node->stop;
done_testing();

# (a failure with the worker stopped would leave the server unable to stop)
END
{
	kill 'CONT', $worker if $worker && !$windows_os;
}
