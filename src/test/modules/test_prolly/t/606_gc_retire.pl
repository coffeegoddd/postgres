
# Copyright (c) 2026, PostgreSQL Global Development Group

# Retiring generations of the chunk store on a versioned database: the
# table's values out of line (value trees its rows name by hash pointers,
# roots on the pages) and its history are kept whatever generation
# they were born in, through a crash right after and on a standby that
# replays it; the settled files' generations check out after each step.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;
$primary->safe_psql('postgres', 'CREATE DATABASE rt');
$primary->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'b', has_streaming => 1);
$standby->start;

sub sql { return $primary->safe_psql('rt', $_[0]); }
sql(q{CREATE EXTENSION test_prolly});
sql(q{CREATE SCHEMA rt VERSIONED;
	CREATE TABLE rt.t (id int PRIMARY KEY, v text);
	INSERT INTO rt.t SELECT g, (SELECT string_agg(md5(g::text || i), '')
		FROM generate_series(1, 100) i) FROM generate_series(1, 200) g;
	SELECT dolt_commit('-Am', 'c0')});
sql(q{SELECT * FROM test_chunk_gc(false)});
my $first = sql(q{SELECT test_chunk_generation()});
for my $c (1 .. 4)
{
	sql(qq{UPDATE rt.t SET v = 'c$c' || repeat('y', 3000) WHERE id % 5 = $c - 1;
		SELECT dolt_commit('-Am', 'c$c')});
	sql(q{SELECT * FROM test_chunk_gc(false)});
}
# A branch's commit, settled, then the branch deleted.
sql(q{SELECT dolt_checkout('-b', 'tmp')});
sql(q{UPDATE rt.t SET v = 'tmp' || repeat('z', 3000) WHERE id <= 50;
	SELECT dolt_commit('-Am', 'tmp')});
sql(q{SELECT dolt_checkout('main')});
sql(q{SELECT * FROM test_chunk_gc(false)});
sql(q{SELECT dolt_branch('-D', 'tmp')});
sql(q{UPDATE rt.t SET v = 'c4b' WHERE id = 2; SELECT dolt_commit('-Am', 'c4b')});
sql(q{SELECT * FROM test_chunk_gc(false)});
my $last = sql(q{SELECT test_chunk_generation()});
my $rows = q{SELECT count(*), sum(length(v)) FROM rt.t};
my $want = sql($rows);
my $history = q{SELECT count(*) FROM dolt_diff('main~4', 'main', NULL::rt.t)};
my $diff = sql($history);
cmp_ok($diff, '>', 0, 'the history differs');

# Every generation but the newest retired: the files of the older ones
# lose what nothing names any more, the rest is kept.
my $older = $last - 1;
my ($kept, $freed) = split /\|/,
  sql(qq{SELECT kept, freed FROM test_chunk_retire($first, $older)});
cmp_ok($kept + $freed, '>=', 0, 'the retire of the old generations runs');
is(sql($rows), $want, 'the table reads the same');
is(sql($history), $diff, 'and so does its history');
cmp_ok(sql(q{SELECT test_chunk_verify_generations()}), '>', 0,
	'the generations check out');

# A standby replays it.
$primary->wait_for_catchup($standby, 'replay');
is($standby->safe_psql('rt', $rows), $want, 'the standby reads the table');
is($standby->safe_psql('rt', $history), $diff, 'and its history');

# A crash right after a retire.
sql(q{UPDATE rt.t SET v = 'c5' WHERE id = 1; SELECT dolt_commit('-Am', 'c5')});
$want = sql($rows);
my $log = sql(q{SELECT count(*) FROM dolt_log('main')});
sql(qq{SELECT * FROM test_chunk_retire($first, $last)});
$primary->stop('immediate');
$primary->start;
is(sql($rows), $want, 'after a crash the table reads the same');
is(sql(q{SELECT count(*) FROM dolt_log('main')}), $log, 'and its log');
cmp_ok(sql(q{SELECT test_chunk_verify_generations()}), '>', 0,
	'the generations check out after the crash');
sql(q{SELECT * FROM test_chunk_gc(true)});
is(sql($rows), $want, 'a full collection after keeps it');

$standby->stop;
$primary->stop;
done_testing();
