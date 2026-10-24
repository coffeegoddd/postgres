# Copyright (c) 2026, PostgreSQL Global Development Group

# dolt_reflog after a collection that keeps the newest sealed segments for
# a logical replication slot: their moves are still recorded, and listed
# from the root the collected segments before them end at.  So a move
# among them that changed no ref (a working set's checkpoint) lists
# nothing, as after a collection that keeps none.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('reflog_gc_slot');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA rs VERSIONED;
	CREATE TABLE rs.t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'base');
	SELECT test_chunk_seal();
});

# A slot from here on: the segments sealed after it are kept, as they are.
sql(q{SELECT pg_create_logical_replication_slot('rs', 'test_decoding')});
sql(q{INSERT INTO rs.t VALUES (1)});
sql(q{SELECT prolly_checkpoint()});
sql(q{SELECT dolt_commit('-am', 'row 1')});
sql(q{SELECT dolt_gc()});

is(sql(q{SELECT count(*) FROM test_chunk_segments()}), '2',
	'the collection kept the sealed segment the slot may decode');
is(sql(q{SELECT string_agg(commit_message, ',') FROM dolt_reflog('main')}), 'row 1',
	"the kept segment's moves listed from the collected ones' root: the checkpoint lists nothing");

sql(q{SELECT pg_drop_replication_slot('rs')});
$node->stop;
done_testing();
