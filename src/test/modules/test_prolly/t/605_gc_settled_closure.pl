
# Copyright (c) 2026, PostgreSQL Global Development Group

# A default collection keeps settled files whole without reading them, so
# whatever a settled file's chunks name must be settled too, or a later
# default collection frees it while it is still named.  Two ways a settled
# chunk came to name one that wasn't:
#
# 1. A repair reattaches a fragment of history below the boundary
#    commits, which are in settled files: what it received must be
#    settled, or the next default collection frees it.
# 2. A collection copies a chunk (here a transfer's) naming one in a
#    segment it keeps as it is, for a logical slot: what it writes then
#    isn't settled, so the next collection, of that segment too, still
#    finds the chunk named.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('settled');
$node->init(allows_streaming => 'logical');
$node->start;

# 1. A repair, then a default collection.
$node->safe_psql('postgres', 'CREATE DATABASE rc');
my $url = 'file://' . PostgreSQL::Test::Utils::tempdir() . '/remote';
sub rc { return $node->safe_psql('rc', $_[0]); }
rc(q{CREATE EXTENSION test_prolly});
rc(q{CREATE SCHEMA rc VERSIONED;
	CREATE TABLE rc.t (id int PRIMARY KEY, v text);
	INSERT INTO rc.t SELECT g, 'v' FROM generate_series(1, 200) g;
	SELECT dolt_commit('-Am', 'd0', '--date', '2020-01-01')});
for my $d (1 .. 4)
{
	rc(qq{UPDATE rc.t SET v = 'd$d' WHERE id % 7 = $d;
		SELECT dolt_commit('-Am', 'd$d', '--date', '2020-01-0} . ($d + 1) . q{')});
}
rc(qq{SELECT dolt_remote('add', 'origin', '$url');
	SELECT dolt_push('origin', 'main')});
my $whole = rc(q{SELECT count(*) FROM test_chunk_reachable()});
rc(q{SELECT commits FROM dolt_history_drop('2020-01-04')});
rc(q{SELECT * FROM prolly_gc(true)});
cmp_ok(rc(q{SELECT count(*) FROM test_chunk_reachable()}), '<', $whole,
	'the severed history is freed');
rc(q{SELECT dolt_history_repair('origin')});
is(rc(q{SELECT count(*) FROM test_chunk_reachable()}), $whole,
	'the repair brings it back');
rc(q{SELECT * FROM prolly_gc(false)});
is(rc(q{SELECT count(*) FROM test_chunk_reachable()}), $whole,
	'a default collection after the repair keeps it');
is(rc(q{SELECT count(*) FROM dolt_diff('main~4', 'main~3', 'rc.t')}), '29',
	'and the repaired history reads');

# 2. A chunk named only from a collected chunk, in a segment kept for a
# logical slot.  The naming chunk comes from another database, as a
# transfer's file would.
$node->safe_psql('postgres', 'CREATE DATABASE ls');
$node->safe_psql('postgres', 'CREATE DATABASE ls_src');
$node->safe_psql('ls', 'CREATE EXTENSION test_prolly');
$node->safe_psql('ls_src', 'CREATE EXTENSION test_prolly');
my $named = $node->safe_psql('ls_src',
	q{SELECT test_chunk_get(test_chunk_put_node(1, ARRAY[test_chunk_put('ls x')]))});
$node->safe_psql('ls',
	q{SELECT pg_create_logical_replication_slot('ls_slot', 'test_decoding')});
my $x = $node->safe_psql('ls', q{SELECT test_chunk_put('ls x')});
$node->safe_psql('ls', q{SELECT test_chunk_seal()});
$node->safe_psql('ls', qq{SELECT test_chunk_table_file(ARRAY['$named'::bytea]);
	SELECT test_chunk_move_root(NULL, test_content_hash('$named'::bytea))});
$node->safe_psql('ls', q{SELECT * FROM test_chunk_gc(false)});
is($node->safe_psql('ls',
		q{SELECT count(*) FROM test_chunk_segments() WHERE chunks > 0}),
	'1', 'the segment with the named chunk is kept for the slot');
$node->safe_psql('ls', q{SELECT pg_drop_replication_slot('ls_slot')});
$node->safe_psql('ls', q{SELECT * FROM test_chunk_gc(false)});
is($node->safe_psql('ls', qq{SELECT test_chunk_has('$x')}), 't',
	'the next collection, of that segment too, keeps the chunk named');
$node->safe_psql('ls', q{SELECT * FROM test_chunk_gc(false)});
is($node->safe_psql('ls', qq{SELECT test_chunk_has('$x')}), 't',
	'and so does the one after');

$node->stop;
done_testing();
