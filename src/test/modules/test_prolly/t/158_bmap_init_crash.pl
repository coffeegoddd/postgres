# Copyright (c) 2026, PostgreSQL Global Development Group

# An allocator's first page is extended before its initialization is logged.
# A crash in between must not leave the database unable to create another
# block-mapped table (#666).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('init_crash');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
autovacuum = off
checkpoint_timeout = 1h
wal_consistency_checking = 'all'
});
$node->start;
my $dboid = $node->safe_psql('postgres',
	q{SELECT oid FROM pg_database WHERE datname = current_database()});
my $allocator = $node->data_dir . "/base/$dboid/9420";

sub allocator_contents
{
	open(my $fh, '<:raw', $allocator) or die "open $allocator: $!";
	local $/;
	my $contents = <$fh>;
	close($fh) or die "close $allocator: $!";
	return $contents;
}

$node->safe_psql('postgres', q{CREATE EXTENSION injection_points;
	CREATE EXTENSION pg_walinspect;
	CREATE SCHEMA app VERSIONED; CHECKPOINT;});

my $session = $node->background_psql('postgres', on_error_stop => 0);
$session->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-alloc-extended', 'wait')});
$session->query_until(qr/creating/, q{
	\echo creating
	BEGIN;
	CREATE TABLE app.interrupted (id int PRIMARY KEY);
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-alloc-extended'})
  or die 'allocator initialization did not wait';

$node->stop('immediate');
eval { $session->quit; };
is(allocator_contents(), "\0" x 8192,
	'the interrupted extension leaves exactly one zero-filled allocator page');
$node->start;
is($node->safe_psql('postgres', q{SELECT to_regclass('app.interrupted') IS NULL}),
	't', 'the interrupted CREATE TABLE did not commit');

# The first creator holds the reused zero page exclusively while the second
# waits to read it.  Once released, both must use the same initialization.
my $start_lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my $first = $node->background_psql('postgres');
my $second = $node->background_psql('postgres');
my $second_pid = $second->query_safe('SELECT pg_backend_pid()');
$first->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-alloc-extended', 'wait')});
$first->query_until(qr/retrying/, q{
	\echo retrying
	CREATE TABLE app.retried (id int PRIMARY KEY);
	INSERT INTO app.retried VALUES (1), (2);
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-alloc-extended'})
  or die 'retried allocator initialization did not wait';
$second->query_until(qr/concurrent/, q{
	\echo concurrent
	CREATE TABLE app.concurrent (id int PRIMARY KEY);
	INSERT INTO app.concurrent VALUES (20);
});
ok($node->poll_query_until('postgres', qq{SELECT count(*) = 1
	FROM pg_stat_activity WHERE pid = $second_pid
	AND wait_event_type = 'Buffer' AND wait_event = 'BufferShared'}),
	'the concurrent creator waits for the allocator header buffer');
$node->safe_psql('postgres',
	q{SELECT injection_points_wakeup('bmap-alloc-extended')});
is($first->query_safe('SELECT sum(id) FROM app.retried'), '3',
	'the first creator completes after retrying initialization');
is($second->query_safe('SELECT id FROM app.concurrent'), '20',
	'the concurrent creator completes without resetting the allocator');
$first->quit;
$second->quit;
my $end_lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
is($node->safe_psql('postgres', qq{
	SELECT count(*) FROM pg_get_wal_records_info('$start_lsn', '$end_lsn')
	WHERE resource_manager = 'BlockMap' AND record_type = 'INIT'
}), '1', 'the concurrent creators emit exactly one allocator INIT record');

# The retried initialization is WAL-logged too: a second crash must keep
# both tables' data and a usable allocator for the next relation.
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', q{
	SET enable_seqscan = off;
	SELECT sum(id) FROM app.retried WHERE id BETWEEN 1 AND 2;
}), '3', 'the retried allocator and index survive another crash');
is($node->safe_psql('postgres', q{
	SET enable_seqscan = off;
	SELECT id FROM app.concurrent WHERE id = 20;
}), '20', 'the concurrent creator table and index survive another crash');
$node->safe_psql('postgres', q{
	CREATE TABLE app.next_table (id int PRIMARY KEY);
	INSERT INTO app.next_table VALUES (10);
});
is($node->safe_psql('postgres', q{SELECT id FROM app.next_table}), '10',
	'the allocator remains usable for another table');

# ERROR also leaves the extended page behind, including its cached buffer.
# A new database gives this case an allocator of its own.
$node->safe_psql('postgres', 'CREATE DATABASE error_retry');
$node->safe_psql('error_retry', q{CREATE EXTENSION injection_points;
	CREATE SCHEMA app VERSIONED});
my ($ret, $out, $err) = $node->psql('error_retry', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-alloc-extended', 'error');
	BEGIN;
	CREATE TABLE app.interrupted (id int PRIMARY KEY);
});
like($err, qr/error triggered for injection point bmap-alloc-extended/,
	'initialization can fail with ERROR after extending the first page');
$node->safe_psql('error_retry', q{
	CREATE TABLE app.retried (id int PRIMARY KEY);
	INSERT INTO app.retried VALUES (4);
});
is($node->safe_psql('error_retry', q{SELECT id FROM app.retried}), '4',
	'the next creator retries the zero-filled page without a restart');

# A zeroed header of an allocator that already owns storage is corruption,
# not an interrupted initialization.  Never reset it and reuse live blocks.
$node->stop;
my $damaged = allocator_contents();
cmp_ok(length($damaged), '>', 8192, 'the allocator has bitmap pages');
my $nalloc = length($damaged) / 8192;
my $nstore = (-s ($node->data_dir . "/base/$dboid/9421")) / 8192;
my $nrefs = (-s ($node->data_dir . "/base/$dboid/9419")) / 8192;
substr($damaged, 0, 8192) = "\0" x 8192;
open(my $fh, '>:raw', $allocator) or die "open $allocator: $!";
print $fh $damaged or die "write $allocator: $!";
close($fh) or die "close $allocator: $!";
$node->start;
($ret, $out, $err) = $node->psql('postgres', q{
	CREATE TABLE app.refused (id int PRIMARY KEY);
});
isnt($ret, 0, 'a damaged allocator with existing storage is refused');
like($err, qr/cannot initialize block map allocator .* with existing storage/,
	'the error identifies the unsafe initialization');
like($err,
	qr/DETAIL:  The allocator header is missing or zeroed, but storage already exists \(allocator: $nalloc blocks, page store: $nstore blocks, reference counts: $nrefs blocks\)\./,
	'the error detail identifies the header damage and existing storage sizes');
like($err, qr/HINT:  Restore the affected database from a known-good backup\./,
	'the error hint explains how to recover');
is(allocator_contents(), $damaged, 'the damaged allocator is not reset');
$node->stop;
done_testing();
