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

# An autovacuum worker's whole visit of the database, from here on: it
# vacuums a table given a row (its tables come before the stores), then
# leaves.
sql(q{CREATE TABLE visits (n int) USING heap WITH
	(autovacuum_vacuum_insert_threshold = 0,
	 autovacuum_vacuum_insert_scale_factor = 0)});
sub autovacuum_visited
{
	my $count = q{SELECT coalesce(autovacuum_count, 0) FROM pg_stat_user_tables
		WHERE relname = 'visits'};
	my $before = sql($count);
	sql('INSERT INTO visits VALUES (1)');
	$node->poll_query_until('mv', "SELECT ($count) > $before")
	  or die 'autovacuum did not visit';
	$node->poll_query_until('postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker' AND datname = 'mv'})
	  or die 'the autovacuum worker did not leave';
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

# Past it, its chunks move, the garbage too.  (Written with autovacuum
# off: the transaction appends its batches as it goes, and a move between
# them would leave the rest in the new segment.)
reload_settings('autovacuum = off');
sql(q{SELECT count(test_chunk_put(test_noise(g::text, 100000)))
	FROM generate_series(1, 20) g});
reload_settings('autovacuum = on');
ok($node->poll_query_until('mv', "SELECT ($files) > 0"),
	'past the threshold, the journal moves to a table file');
ok($node->poll_query_until('mv', q{SELECT value > 0 FROM pg_stat_prolly
	WHERE name = 'gc_moves'}),
	'the move is counted, as a move and not a collection');
is(sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'gc_collections'}), '0',
	'no collection was made');
is( sql(
		q{SELECT string_agg(value::text, ',' ORDER BY name) FROM pg_stat_prolly
	WHERE name IN ('gc_chunks_kept', 'gc_chunks_freed', 'gc_bytes_freed',
				   'gc_window_usec')}),
	'0,0,0,0',
	'... nor in what collections kept and freed');
is(sql(q{SELECT string_agg(chunks::text, ',') FROM test_chunk_segments()}), '0',
	'leaving one segment, empty');
is(sql(q{SELECT string_agg(settled::text, ',') FROM test_chunk_table_files()}), 'false',
	'a table file not settled');
my $fresh = q{SELECT count(*) FROM generate_series(1, 20) g
	WHERE test_chunk_has(test_content_hash(test_noise(g::text, 100000)))};
is(sql($fresh), '20', 'a new backend reads the chunks moved');
is(sql("SELECT test_chunk_has('$dead')"), 't', 'and the garbage: nothing is freed');

$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('mv', $fresh), '20', 'a standby reads them too');

# The journal's sealed segments count: two of half the threshold each,
# neither past it alone, move.  The table file moved before is left as it
# is.
my $first = sql(q{SELECT name FROM test_chunk_table_files()});
reload_settings('autovacuum = off');
sql(q{SELECT count(test_chunk_put(test_noise(g::text || 's', 50000)))
	FROM generate_series(1, 20) g});
sql('SELECT test_chunk_seal()');
sql(q{SELECT count(test_chunk_put(test_noise(g::text || 't', 50000)))
	FROM generate_series(1, 20) g});
is(sql(q{SELECT count(*) FROM test_chunk_segments() WHERE chunks > 0}), '2',
	'two segments with chunks');
reload_settings('autovacuum = on');
ok($node->poll_query_until('mv', "SELECT ($files) = 2"),
	'the sealed segment counted, the journal moves');
is(sql(qq{SELECT count(*) FROM test_chunk_table_files() WHERE name = '$first'}), '1',
	'the table file moved before isn\'t rewritten');

# A move frees nothing, so it waits for no transaction open as it begins
# before its swap (a collection would): its chunks move to a table file
# with one left open (its retire still waits for it, as any does).
my $moved = sql($files);
my $open = $node->background_psql('mv');
$open->query_safe(q{BEGIN; SELECT test_chunk_has('00')});
reload_settings('autovacuum = off');
sql(q{SELECT count(test_chunk_put(test_noise(g::text || 'o', 100000)))
	FROM generate_series(1, 20) g});
reload_settings('autovacuum = on');
ok($node->poll_query_until('mv', "SELECT ($files) > $moved"),
	'a move swaps with a transaction left open');
$open->query_safe('COMMIT');
$open->quit;

# With the threshold -1, it doesn't move.
reload_settings('prolly.journal_move_threshold = -1');
my $before = sql($files);
sql(q{SELECT count(test_chunk_put(test_noise(g::text || 'b', 100000)))
	FROM generate_series(1, 20) g});
autovacuum_visited();
is(sql($files), $before, 'with the threshold -1, the journal stays');

# A default collection collects the moved files' garbage.
sql('SELECT test_chunk_gc(false) IS NOT NULL');
is(sql("SELECT test_chunk_has('$dead')"), 'f',
	'a default collection frees the garbage moved');

# Past prolly.conjoin_max_files table files, autovacuum conjoins the
# smallest, and the files left are no more than that; every chunk reads.
my $conjoins = q{SELECT value FROM pg_stat_prolly WHERE name = 'conjoins'};
my $conjoins_before = sql($conjoins);
reload_settings('prolly.conjoin_max_files = 3');
my $nfiles = q{SELECT count(*) FROM test_chunk_table_files()};
sql(qq{SELECT test_chunk_table_file(ARRAY[test_noise('file $_', 100)])}) for 1 .. 5;
ok($node->poll_query_until('mv', "SELECT ($nfiles) <= 3"),
	'past the limit, autovacuum conjoins the smallest files');
is(sql(q{SELECT count(*) FROM generate_series(1, 5) g
	WHERE test_chunk_has(test_content_hash(test_noise('file ' || g, 100)))}), '5',
	'every conjoined chunk reads');
ok($node->poll_query_until('mv', "SELECT ($conjoins) > $conjoins_before"),
	'the conjoin is counted');
reload_settings('prolly.conjoin_max_files = -1');
my $kept = sql($nfiles);
sql(qq{SELECT test_chunk_table_file(ARRAY[test_noise('more $_', 100)])}) for 1 .. 4;
autovacuum_visited();
is(sql($nfiles), $kept + 4, 'with the limit -1, files aren\'t conjoined');

$standby->stop;
$node->stop;
done_testing();
