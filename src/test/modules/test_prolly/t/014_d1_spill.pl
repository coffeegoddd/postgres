# Copyright (c) 2026, PostgreSQL Global Development Group

# Engine D1's private writes past prolly_write_set_memory are spilled to
# intermediate trees, so a transaction's memory stays bounded; its reads,
# rollbacks to savepoints and commit still see exactly its writes, also in
# one statement that reads the table it writes and with others committing
# meanwhile; a cursor from before a spill that would need versions the
# spill dropped says so.  PREPARE TRANSACTION keeps prolly writes (and their
# row locks) across a crash, and COMMIT PREPARED replays them onto what
# others committed meanwhile, on a standby too; ROLLBACK PREPARED drops
# them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('spill');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
default_table_access_method = prolly_d1
max_prepared_transactions = 10
});
$node->start;

$node->safe_psql('postgres', q{
	CREATE TABLE sp (id int PRIMARY KEY, v text);
	CREATE INDEX sp_v ON sp USING btree (v);
	CREATE TABLE pp (id int PRIMARY KEY, v int);
	INSERT INTO pp SELECT g, 0 FROM generate_series(1, 10) g;
});

my $s1 = $node->background_psql('postgres', on_error_stop => 0);

# The memory of the transaction's private writes, in bytes.
my $mem_sql = q{SELECT coalesce(sum(total_bytes), 0) FROM pg_backend_memory_contexts
	WHERE name LIKE 'prolly d1 %'};

# Another session commits to the table after this transaction's first
# write (before its many writes lock the whole table); the commit is
# replayed onto it.
$s1->query_safe(q{SET prolly_write_set_memory = '64kB'; BEGIN;
	INSERT INTO sp VALUES (0, repeat('x', 50));});
$node->safe_psql('postgres', q{INSERT INTO sp VALUES (-1, 'other')});
$s1->query_safe(q{
	INSERT INTO sp SELECT g, repeat('x', 50) FROM generate_series(1, 29999) g;});
cmp_ok($s1->query_safe($mem_sql), '<', 2 * 1024 * 1024,
	'a transaction\'s write set stays near prolly_write_set_memory');
is($s1->query_safe('SELECT count(*) FROM sp WHERE id >= 0'), '30000',
	'the transaction reads its spilled writes');
is($s1->query_safe(q{SET enable_seqscan = off;
	SELECT count(*) FROM sp WHERE v = repeat('x', 50); RESET enable_seqscan;}),
	'30000', 'and finds them through an index');

# Rolling back to a savepoint drops what was spilled after it.
$s1->query_safe(q{SAVEPOINT s;
	UPDATE sp SET v = 'y' WHERE id <= 20000;
	INSERT INTO sp SELECT g, 'z' FROM generate_series(30001, 40000) g;});
is($s1->query_safe(q{SELECT count(*) || ':' || count(*) FILTER (WHERE v = 'y') FROM sp WHERE id >= 0}),
	'40000:20001', 'writes after a savepoint are seen');
$s1->query_safe('ROLLBACK TO s');
is($s1->query_safe(q{SELECT count(*) || ':' || count(*) FILTER (WHERE v = 'y') FROM sp WHERE id >= 0}),
	'30000:0', 'rolling back to the savepoint drops them, spilled or not');

# One statement reading the table it writes sees none of its own rows.
$s1->query_safe('INSERT INTO sp SELECT id + 100000, v FROM sp WHERE id >= 0');
is($s1->query_safe('SELECT count(*) FROM sp WHERE id >= 0'), '60000',
	'a statement spilling its own writes doesn\'t read them');
$s1->query_safe('COMMIT');
is($node->safe_psql('postgres',
	q{SELECT count(*) || ':' || min(id) || ':' || max(id) FROM sp}),
	'60001:-1:129999', 'the commit keeps both its writes and the other\'s');

# A cursor that began before a spill can't be served versions it dropped.
$s1->query_safe(q{BEGIN; DECLARE c CURSOR FOR SELECT * FROM sp;
	UPDATE sp SET v = 'w' WHERE id = 1;
	INSERT INTO sp SELECT g, 'q' FROM generate_series(200001, 230000) g;});
$s1->query('FETCH 1 FROM c');
like($s1->{stderr}, qr/cannot read a prolly table as of an earlier command/,
	'a cursor from before the spill says it can\'t be served');
$s1->{stderr} = '';
$s1->query_safe('ROLLBACK; RESET prolly_write_set_memory;');

# PREPARE TRANSACTION, then a crash, then COMMIT PREPARED after others
# committed to the same table.
$s1->query_safe(q{BEGIN; UPDATE pp SET v = 1 WHERE id = 1;
	INSERT INTO pp VALUES (100, 100); PREPARE TRANSACTION 'p1';});
is($node->safe_psql('postgres', 'SELECT count(*) FROM pp WHERE id = 100'),
	'0', 'a prepared transaction\'s rows aren\'t seen');
$node->safe_psql('postgres', q{INSERT INTO pp VALUES (200, 200);
	UPDATE pp SET v = 2 WHERE id = 2;});
my ($ret, $out, $err) = $node->psql('postgres', q{
	SET lock_timeout = '200ms'; UPDATE pp SET v = 9 WHERE id = 1;});
like($err, qr/lock timeout/, 'a prepared transaction keeps its row locks');
$s1->quit;
$node->stop('immediate');
$node->start;
$node->safe_psql('postgres', q{COMMIT PREPARED 'p1'});
is($node->safe_psql('postgres',
	q{SELECT array_agg(id || ':' || v ORDER BY id) FROM pp WHERE id IN (1, 2, 100, 200)}),
	'{1:1,2:2,100:100,200:200}',
	'COMMIT PREPARED after a crash keeps its writes and others\' since');

# ROLLBACK PREPARED drops them.
$node->safe_psql('postgres', q{BEGIN; DELETE FROM pp WHERE id = 100;
	PREPARE TRANSACTION 'p2';});
$node->safe_psql('postgres', q{ROLLBACK PREPARED 'p2'});
is($node->safe_psql('postgres', 'SELECT count(*) FROM pp WHERE id = 100'),
	'1', 'ROLLBACK PREPARED drops its writes');

# A prepared transaction that spilled, committed on the primary, replayed
# on a standby.
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('spill_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;
$node->safe_psql('postgres', q{SET prolly_write_set_memory = '64kB'; BEGIN;
	INSERT INTO pp SELECT g, g FROM generate_series(1000, 21000) g;
	PREPARE TRANSACTION 'p3';});
$node->safe_psql('postgres', q{COMMIT PREPARED 'p3'});
$node->wait_for_replay_catchup($standby);
is($node->safe_psql('postgres', 'SELECT count(*) FROM pp'), '20013',
	'a prepared transaction that spilled commits every row');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM pp'), '20013',
	'and a standby replays it');

# Rolling back to a savepoint compares keys of the writes it undoes, while
# the catalogs can't be read: a backend whose caches are cold (a restart)
# must have what that takes already (Postgres's 018_wal_optimize crashed).
$standby->stop;
$node->restart;
is($node->safe_psql('postgres', q{
	BEGIN;
	CREATE TABLE sp_cold (c int PRIMARY KEY);
	SAVEPOINT q;
	INSERT INTO sp_cold VALUES (1);
	ROLLBACK TO q;
	COMMIT;
	SELECT count(*) FROM sp_cold;}), '0',
	'a rollback to a savepoint in a fresh backend');

$node->stop;
done_testing();
