# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum bounds the chunk journal: once the segment appends go to
# passes prolly.journal_move_threshold, and no collection was due, its
# chunks move into a settled table file, garbage and all, and a new
# backend reads them there; not before, nor with the threshold -1.  A
# standby follows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
prolly.gc_journal_threshold = -1
prolly.journal_move_threshold = 1MB
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE mv');
$node->safe_psql('mv', 'CREATE EXTENSION test_prolly');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('move_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('mv', $_[0]); }

my $files = q{SELECT count(*) FROM test_chunk_table_files()};
is(sql('SHOW prolly.journal_move_threshold'), '1MB', 'the threshold is a setting');

# Below the threshold, the journal stays.
my $dead = sql(q{SELECT test_chunk_put('garbage, never named')});
sleep(3);
is(sql($files), '0', 'below the threshold, no table file');

# Past it, its chunks move, the garbage too.
sql(q{SELECT count(test_chunk_put(convert_to(repeat(g::text, 100000), 'UTF8')))
	FROM generate_series(1, 20) g});
ok($node->poll_query_until('mv', "SELECT ($files) > 0"),
	'past the threshold, the journal moves to a table file');
is(sql(q{SELECT string_agg(chunks::text, ',') FROM test_chunk_segments()}), '0',
	'leaving one segment, empty');
my $fresh = q{SELECT count(*) FROM generate_series(1, 20) g
	WHERE test_chunk_has(encode(sha256(convert_to(repeat(g::text, 100000), 'UTF8')), 'hex'))};
is(sql($fresh), '20', 'a new backend reads the chunks moved');
is(sql("SELECT test_chunk_has('$dead')"), 't', 'and the garbage: nothing is freed');

$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('mv', $fresh), '20', 'a standby reads them too');

# With the threshold -1, it doesn't move.
$node->append_conf('postgresql.conf', 'prolly.journal_move_threshold = -1');
$node->reload;
$node->poll_query_until('mv', q{SELECT current_setting('prolly.journal_move_threshold') = '-1'})
  or die 'the setting did not reload';
my $before = sql($files);
sql(q{SELECT count(test_chunk_put(convert_to(repeat(g::text, 100000) || 'b', 'UTF8')))
	FROM generate_series(1, 20) g});
sleep(3);
is(sql($files), $before, 'with the threshold -1, the journal stays');

$standby->stop;
$node->stop;
done_testing();
