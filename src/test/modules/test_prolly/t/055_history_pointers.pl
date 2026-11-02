# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit's chunks and its pointer's move are one: a crash with the
# transaction open leaves neither, a committed one survives it, and a hot
# standby has them as the commit replays.  Dropping the pointer has the
# store's background freer free the chunk.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('history');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION put(d bytea) RETURNS bool LANGUAGE sql
		AS $$SELECT test_chunk_put('pg_history.chunks', 'pg_history.chunk_index', h(d), d)$$;
});
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('history_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

# a commit: a chunk, and a pointer to it
$node->safe_psql('postgres', q{
	BEGIN;
	SELECT put('\x01');
	INSERT INTO pg_history.pointers VALUES ('main', h('\x01'));
	COMMIT;
});

# and one cut short by a crash: a chunk, and the pointer moved to it
my $open = $node->background_psql('postgres');
$open->query_safe(q{BEGIN});
$open->query_safe(q{SELECT put('\x02')});
$open->query_safe(q{UPDATE pg_history.pointers SET hash = h('\x02') WHERE name = 'main'});
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$open->{run}->finish;
$node->start;

my $state = q{SELECT (SELECT hash = h('\x01') FROM pg_history.pointers WHERE name = 'main')
	|| ' ' || test_chunk_has('pg_history.chunks', 'pg_history.chunk_index', h('\x01'))
	|| ' ' || test_chunk_has('pg_history.chunks', 'pg_history.chunk_index', h('\x02'))
	|| ' ' || (SELECT roots FROM test_chunk_index_entry('pg_history.chunk_index', h('\x01')))};
is($node->safe_psql('postgres', $state), 'true true false 1',
	'after the crash: the committed chunk and pointer, and nothing of the other');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $state), 'true true false 1',
	'the standby has them as the commit replayed');
is($node->safe_psql('postgres', q{SELECT ok FROM test_chunk_check('pg_history.chunks', 'pg_history.chunk_index')}),
	't', 'the audit agrees');

# on the standby, a commit's chunk and pointer move appear together as its
# commit replays: the records before it replayed, neither; the commit
# replayed, both
my $mid = $node->background_psql('postgres');
$mid->query_safe(q{BEGIN});
$mid->query_safe(q{SELECT put('\x03')});
$mid->query_safe(q{UPDATE pg_history.pointers SET hash = h('\x03') WHERE name = 'main'});
# (another's commit, its WAL after the open transaction's records)
$node->safe_psql('postgres', 'CREATE TABLE flushed ()');
$node->wait_for_replay_catchup($standby);
my $at3 = q{SELECT (SELECT hash = h('\x03') FROM pg_history.pointers WHERE name = 'main')
	|| ' ' || test_chunk_has('pg_history.chunks', 'pg_history.chunk_index', h('\x03'))};
is($standby->safe_psql('postgres', $at3), 'false false',
	'its records replayed, not its commit: neither the chunk nor the pointer moved');
$mid->query_safe(q{COMMIT});
$mid->quit;
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $at3), 'true true', 'its commit replayed: both');
$node->safe_psql('postgres', q{UPDATE pg_history.pointers SET hash = h('\x01') WHERE name = 'main'});

# the pointer dropped, VACUUM of the store's index has a freer free the chunk
$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'main'});
$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
ok($node->poll_query_until('postgres',
		q{SELECT test_chunk_has('pg_history.chunks', 'pg_history.chunk_index', h('\x01'))}, 'f'),
	'the pointer dropped, the store frees its chunk in the background');

$standby->stop;
$node->stop;
done_testing();
