# Copyright (c) 2026, PostgreSQL Global Development Group

# Engine D1 reads the root of the newest commit a snapshot sees: a
# REPEATABLE READ transaction keeps reading what it saw first, through
# sequential and index scans, even when its snapshot was taken by a heap
# table before the prolly table was first read; READ COMMITTED reads each
# statement's commits; an update of a row changed since the snapshot is a
# serialization failure.  The roots older snapshots need are kept while
# room allows, and a snapshot whose root was given up gets "snapshot too
# old".  A standby's snapshots do the same with the commits it replays.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('snapshots');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', "default_table_access_method = prolly_d1\n");
$node->start;

$node->safe_psql('postgres', q{
	CREATE TABLE h (id int) USING heap;
	INSERT INTO h VALUES (1);
	CREATE TABLE t (id int PRIMARY KEY, v int);
	INSERT INTO t VALUES (1, 10), (2, 20);
	CREATE TABLE u (id int, v int);
	CREATE INDEX u_id ON u USING btree (id);
	INSERT INTO u SELECT g, g FROM generate_series(1, 100) g;
});

my $s1 = $node->background_psql('postgres', on_error_stop => 0);

# The snapshot is taken by a heap table; the prolly tables are first read
# after other transactions committed to them.
$s1->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT * FROM h;');
$node->safe_psql('postgres', q{
	UPDATE t SET v = v * 100 WHERE id = 1;
	INSERT INTO t VALUES (3, 30);
	DELETE FROM u WHERE id <= 50;
});
is($s1->query_safe(q{SELECT array_agg(id || ':' || v ORDER BY id) FROM t}),
	'{1:10,2:20}', 'REPEATABLE READ reads the commit its snapshot saw');
is($s1->query_safe('SELECT count(*) FROM u'), '100',
	'a sequential scan reads the snapshot\'s root');
is($s1->query_safe(q{SET enable_seqscan = off;
	SELECT count(*) FROM u WHERE id BETWEEN 1 AND 60;}), '60',
	'an index scan reads the snapshot\'s root');
$node->safe_psql('postgres', 'INSERT INTO t VALUES (4, 40)');
is($s1->query_safe('SELECT count(*) FROM t'), '2',
	'later scans of the snapshot read the same root');

# Updating a row another transaction changed after the snapshot fails.
$s1->query('UPDATE t SET v = v + 1 WHERE id = 1');
like($s1->{stderr}, qr/could not serialize access due to concurrent update/,
	'updating a row changed since the snapshot is a serialization failure');
$s1->{stderr} = '';
$s1->query_safe('ROLLBACK');

# READ COMMITTED: each statement reads the commits before it.
$s1->query_safe('BEGIN; SELECT count(*) FROM t;');
$node->safe_psql('postgres', 'INSERT INTO t VALUES (5, 50)');
is($s1->query_safe('SELECT count(*) FROM t'), '5',
	'READ COMMITTED reads commits made since the last statement');
$s1->query_safe('COMMIT');

# Room for replaced roots: with two kept, many commits leave no snapshot
# without its root while no old snapshot is open...
$node->append_conf('postgresql.conf', "debug_prolly_max_root_history = 2\n");
$node->reload;
$node->safe_psql('postgres', "INSERT INTO t VALUES ($_, 0)") for (10 .. 19);
is($node->safe_psql('postgres', 'SELECT count(*) FROM t'), '15',
	'roots no snapshot needs are dropped to make room');

# ... and an old snapshot whose root was given up says so.
$s1->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT * FROM h;');
$node->safe_psql('postgres', "INSERT INTO t VALUES ($_, 0)") for (20 .. 24);
$s1->query('SELECT count(*) FROM t');
like($s1->{stderr}, qr/snapshot too old/,
	'a snapshot whose root was given up gets "snapshot too old"');
$s1->{stderr} = '';
$s1->query_safe('ROLLBACK');
$node->append_conf('postgresql.conf', "debug_prolly_max_root_history = 0\n");
$node->reload;

# A standby's snapshots read the commits it had replayed when taken.
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('snapshots_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;
$node->wait_for_replay_catchup($standby);
my $r1 = $standby->background_psql('postgres');
$r1->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT * FROM h;');
$node->safe_psql('postgres', 'DELETE FROM t');
$node->wait_for_replay_catchup($standby);
is($r1->query_safe('SELECT count(*) FROM t'), '20',
	'a standby snapshot reads the root it saw, not a commit replayed later');
$r1->query_safe('COMMIT');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'), '0',
	'a new standby snapshot reads the replayed commit');

$r1->quit;
$s1->quit;
$standby->stop;
$node->stop;
done_testing();
