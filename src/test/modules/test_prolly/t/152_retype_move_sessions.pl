# Copyright (c) 2026, PostgreSQL Global Development Group

# A move across a column's change of type, beside other sessions.  A
# partitioned table emptied at once (#492) is locked before its partitions,
# as a query on it locks them, so a query waiting on the table doesn't
# deadlock with the move.  And a table is emptied at once only when the
# ALTER changing the type rewrites it anyway: a change the ALTER makes in
# place (varchar(10) to varchar(20)) leaves a snapshot from before the move
# seeing the rows, as the ALTER alone would.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('retype_sessions');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
deadlock_timeout = '100ms'
});
$node->start;

my $sql = sub { return $node->safe_psql('postgres', $_[0]); };

$sql->(q{
	CREATE SCHEMA rt VERSIONED;
	CREATE TABLE rt.pt (id int PRIMARY KEY, w int) PARTITION BY RANGE (id);
	CREATE TABLE rt.pt1 PARTITION OF rt.pt FOR VALUES FROM (0) TO (1000);
	CREATE TABLE rt.pt2 PARTITION OF rt.pt FOR VALUES FROM (1000) TO (2000);
	INSERT INTO rt.pt SELECT g, g FROM generate_series(1, 1999) g;
	CREATE TABLE rt.vc (id int PRIMARY KEY, s varchar(10));
	INSERT INTO rt.vc SELECT g, 's' || g FROM generate_series(1, 300) g;
	CREATE TABLE rt.t (id int PRIMARY KEY, w int);
	INSERT INTO rt.t SELECT g, g FROM generate_series(1, 300) g;
	SELECT dolt_commit('-Am', 'int');
});
my $c1 = $sql->(q{SELECT dolt_hashof('HEAD')});
$sql->(q{
	ALTER TABLE rt.pt ALTER COLUMN w TYPE bigint;
	SELECT dolt_commit('-am', 'pt bigint');
});
my $c2 = $sql->(q{SELECT dolt_hashof('HEAD')});
$sql->(q{
	ALTER TABLE rt.vc ALTER COLUMN s TYPE varchar(20);
	ALTER TABLE rt.t ALTER COLUMN w TYPE bigint;
	SELECT dolt_commit('-am', 'varchar(20), t bigint');
});
my $c3 = $sql->(q{SELECT dolt_hashof('HEAD')});
$sql->("SELECT dolt_reset('--hard', '$c1')");

# A session holding the partitioned table (as a query on it does, before
# it locks the partitions), while the move waits for it.
my $reader = $node->background_psql('postgres', on_error_stop => 0);
$reader->query_safe(q{BEGIN; LOCK TABLE ONLY rt.pt IN ACCESS SHARE MODE});
my $mover = $node->background_psql('postgres', on_error_stop => 0);
my $pid = $mover->query_safe('SELECT pg_backend_pid()');
$mover->query_until(qr/moving/, qq{
	\\echo moving
	SELECT dolt_reset('--hard', '$c2');
});
$node->poll_query_until('postgres', qq{SELECT count(*) > 0 FROM pg_locks
	WHERE pid = $pid AND NOT granted AND relation = 'rt.pt'::regclass})
  or die 'the move did not wait for the table';
is($sql->(qq{SELECT count(*) FROM pg_locks WHERE pid = $pid AND granted
	AND mode = 'AccessExclusiveLock'
	AND relation IN ('rt.pt1'::regclass, 'rt.pt2'::regclass)}), '0',
	'the move waiting for the partitioned table holds none of its partitions');
is($reader->query_safe('SELECT count(*) FROM rt.pt1'), '999',
	'a query on a partition then reads it, no deadlock');
$reader->query_safe('COMMIT');
$mover->quit;
is($sql->(q{SELECT format_type(atttypid, atttypmod) FROM pg_attribute
	WHERE attrelid = 'rt.pt'::regclass AND attname = 'w'}), 'bigint',
	'and the move made after it');
is($sql->(q{SELECT count(*) || ':' || sum(w) FROM rt.pt}), '1999:1999000',
	'with every row');

# A snapshot taken before a move whose ALTER doesn't rewrite the table
# (rt.vc) still sees its rows; one whose ALTER rewrites it (rt.t) sees it
# empty, as after the ALTER alone (or a TRUNCATE).
$reader->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1});
$sql->("SELECT dolt_reset('--hard', '$c3')");
is($reader->query_safe('SELECT count(*) FROM rt.vc'), '300',
	'a snapshot from before an in-place retype sees the rows');
is($reader->query_safe('SELECT count(*) FROM rt.t'), '0',
	'a snapshot from before a rewriting retype sees the table empty');
$reader->query_safe('COMMIT');
is($sql->(q{SELECT count(*) FROM rt.vc}) . ',' . $sql->(q{SELECT count(*) FROM rt.t}),
	'300,300', 'after it, every row');
$reader->quit;

$node->stop;
done_testing();
