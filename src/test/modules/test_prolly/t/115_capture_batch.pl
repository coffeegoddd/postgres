# Copyright (c) 2026, PostgreSQL Global Development Group

# A worker far behind applies what it's read as it goes, rather than hold it all, and a map is written
# a bounded batch at a time: nothing's lost or counted twice for that, across a crash or otherwise.

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
	my ($n) = @_;
	my @script;
	for my $i (1 .. $n)
	{
		push @script, "INSERT INTO keyed VALUES ($i + (SELECT coalesce(max(id), 0) FROM keyed), repeat('k', 800));",
		  "INSERT INTO nokey VALUES ($i % 7, repeat('n', 800));";
	}
	run(join("\n", @script));
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
	skip 'injection points not supported by this build', 3 if $ENV{enable_injection_points} ne 'yes' || $windows_os;
	$node->safe_psql('postgres', "CREATE EXTENSION injection_points; SELECT injection_points_attach('capture-applied', 'wait')");
	caught_up();
	kill 'STOP', $worker;
	write_many(300);
	kill 'CONT', $worker;
	$node->wait_for_event('capture worker', 'capture-applied');
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-applied')});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($worker)");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker' AND pid <> $worker})
	  or die 'no worker came back';
	$worker = $node->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'capture worker'});
	same_map('keyed', 'killed between batches, with a key');
	same_map('nokey', 'and with none');
	$node->safe_psql('postgres', 'DROP EXTENSION injection_points');
	pass('and on it goes');
}

# one transaction, more than a batch of a table's map, with rows the same over the edge of one
run(q{BEGIN; INSERT INTO keyed SELECT g + 100000, 'one transaction' FROM generate_series(1, 3500) g; UPDATE keyed SET v = 'again' WHERE id > 100000; COMMIT});
same_map('keyed', 'one transaction, many batches, with a key');
run(q{BEGIN; INSERT INTO nokey SELECT 1000, 'same' FROM generate_series(1, 2500); DELETE FROM nokey WHERE a = 1000 AND ctid IN (SELECT ctid FROM nokey WHERE a = 1000 LIMIT 700); COMMIT});
same_map('nokey', 'and with none, the same row over and over');

# what's in a batch already written is in the map, for a value an update left out of line
run(q{CREATE TABLE out_of_line (id integer PRIMARY KEY, n integer, v text); ALTER TABLE out_of_line ALTER v SET STORAGE EXTERNAL;
	INSERT INTO out_of_line VALUES (1, 0, repeat('old', 5000))});
caught_up();
run(q{BEGIN; UPDATE out_of_line SET v = repeat('new', 5000) WHERE id = 1; INSERT INTO out_of_line SELECT g + 10, 0, 'x' FROM generate_series(1, 2500) g; UPDATE out_of_line SET n = 1 WHERE id = 1; COMMIT});
same_map('out_of_line', 'changed, a batch written, and left');
run(q{BEGIN; INSERT INTO out_of_line VALUES (2, 0, repeat('two', 5000)); UPDATE out_of_line SET n = 1 WHERE id = 2; UPDATE out_of_line SET id = 3 WHERE id = 2; COMMIT});
same_map('out_of_line', 'and in the same batch');

# a batch of bytes, not rows
run(q{CREATE TABLE wide (id integer PRIMARY KEY, v text); BEGIN; INSERT INTO wide SELECT g, repeat(g::text, 400000) FROM generate_series(1, 30) g; COMMIT});
same_map('wide', 'batches by size');

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

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal/m, 'nothing crashed');
$node->stop;
done_testing();

# (a failure with the worker stopped would leave the server unable to stop)
END
{
	kill 'CONT', $worker if $worker && !$windows_os;
}
