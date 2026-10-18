# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum bounds the chunk journal: once its segments pass
# prolly.journal_move_threshold, its chunks move into a table file, garbage
# and all, and a new backend reads them there; not before, nor with the
# threshold -1.  The table file isn't settled, so the next default
# collection collects its garbage, and a later move leaves it as it is.  A
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

# Settings reloaded, and no autovacuum worker left that started before.
sub reload_settings
{
	my ($conf) = @_;
	$node->append_conf('postgresql.conf', $conf);
	$node->reload;
	my $at = sql('SELECT clock_timestamp()');
	$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker' AND backend_start < '$at'})
	  or die 'autovacuum workers did not restart';
}

my $files = q{SELECT count(*) FROM test_chunk_table_files()};
is(sql('SHOW prolly.journal_move_threshold'), '1MB', 'the threshold is a setting');
my ($ret, $out, $err) = $node->psql('mv',
	'ALTER SYSTEM SET prolly.journal_move_threshold = 0');
like($err, qr/invalid value for parameter "prolly.journal_move_threshold": 0/,
	'0 is refused: -1 turns moving off');

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
is(sql(q{SELECT string_agg(settled::text, ',') FROM test_chunk_table_files()}), 'false',
	'a table file not settled');
my $fresh = q{SELECT count(*) FROM generate_series(1, 20) g
	WHERE test_chunk_has(encode(sha256(convert_to(repeat(g::text, 100000), 'UTF8')), 'hex'))};
is(sql($fresh), '20', 'a new backend reads the chunks moved');
is(sql("SELECT test_chunk_has('$dead')"), 't', 'and the garbage: nothing is freed');

$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('mv', $fresh), '20', 'a standby reads them too');

# The journal's sealed segments count: two of half the threshold each,
# neither past it alone, move.  The table file moved before is left as it
# is.
my $first = sql(q{SELECT name FROM test_chunk_table_files()});
reload_settings('autovacuum = off');
sql(q{SELECT count(test_chunk_put(convert_to(repeat(g::text, 50000) || 's', 'UTF8')))
	FROM generate_series(1, 20) g});
sql('SELECT test_chunk_seal()');
sql(q{SELECT count(test_chunk_put(convert_to(repeat(g::text, 50000) || 't', 'UTF8')))
	FROM generate_series(1, 20) g});
is(sql(q{SELECT count(*) FROM test_chunk_segments() WHERE chunks > 0}), '2',
	'two segments with chunks');
reload_settings('autovacuum = on');
ok($node->poll_query_until('mv', "SELECT ($files) = 2"),
	'the sealed segment counted, the journal moves');
is(sql(qq{SELECT count(*) FROM test_chunk_table_files() WHERE name = '$first'}), '1',
	'the table file moved before isn\'t rewritten');

# With the threshold -1, it doesn't move.
reload_settings('prolly.journal_move_threshold = -1');
my $before = sql($files);
sql(q{SELECT count(test_chunk_put(convert_to(repeat(g::text, 100000) || 'b', 'UTF8')))
	FROM generate_series(1, 20) g});
sleep(3);
is(sql($files), $before, 'with the threshold -1, the journal stays');

# A default collection collects the moved files' garbage.
sql('SELECT test_chunk_gc(false) IS NOT NULL');
is(sql("SELECT test_chunk_has('$dead')"), 'f',
	'a default collection frees the garbage moved');

$standby->stop;
$node->stop;
done_testing();
