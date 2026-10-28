# Copyright (c) 2026, PostgreSQL Global Development Group

# The history window's seals (a default collection at each batch boundary
# passed since the store's newest generation ended, below the size
# thresholds) run once a boundary, not at every visit: a collection that
# settled nothing new (a store holding nothing live) still ends its
# generation, the time the next seal is counted from.  And with
# prolly.gc_journal_threshold -1, which turns the default collections off,
# there are none.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('window_seal_timing');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
prolly.gc_journal_threshold = 1MB
prolly.gc_growth_scale_factor = 0
prolly_fold_worker = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE ws');
$node->safe_psql('ws', q{CREATE EXTENSION test_prolly;
	ALTER DATABASE ws SET prolly.history_batch = '1 day'});

sub sql { return $node->safe_psql('ws', $_[0]); }
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }

# The first seal: the store's first generation ends, though nothing in it
# is live.
my $first = sql(q{SELECT test_chunk_put('the first generation''s')});
ok($node->poll_query_until('ws', qq{SELECT NOT test_chunk_has('$first')}),
	'a seal ends the first generation');
is(sql(q{SELECT count(*) FROM test_chunk_epochs()}), '1',
	'its end is recorded though it settled nothing');

# No boundary of a day passes in the visits after: nothing below the
# threshold is collected.
my $dead = sql(q{SELECT test_chunk_put('dead before the next boundary')});
sleep(4);
is(has($dead), 't', 'no seal at every visit after');

# Seals due every second, with the threshold -1: none runs.
$node->append_conf('postgresql.conf', 'prolly.gc_journal_threshold = -1');
$node->reload;
$node->poll_query_until('ws',
	q{SELECT current_setting('prolly.gc_journal_threshold') = '-1'})
  or die 'the setting did not reload';
sql(q{ALTER DATABASE ws SET prolly.history_batch = '1 second'});
sleep(4);
is(has($dead), 't', 'with the threshold -1, no seal collects');

# The threshold back: the next boundary's seal collects it.
$node->append_conf('postgresql.conf', 'prolly.gc_journal_threshold = 1MB');
$node->reload;
ok($node->poll_query_until('ws', qq{SELECT NOT test_chunk_has('$dead')}),
	'with it back, a boundary\'s seal collects');

$node->stop;
done_testing();
