# Copyright (c) 2026, PostgreSQL Global Development Group

# Values stored by content, end to end: a slice of a 100 MB value reads
# the nodes that hold it and the path to them, whether or not TOAST
# compressed it before moving it out; a query whose snapshot still sees a
# row another session deleted reads its value after a collection swapped
# the store's files, and the collection waits for it before removing
# them; a standby reads the values, through a collection on the primary;
# they survive a crash; pg_dump and restore carry them.  (The 100 MB
# values are dropped after the slices: the rest uses smaller ones.)

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('acceptance');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql('CREATE EXTENSION test_prolly');
sql('CREATE SCHEMA a VERSIONED');
sql('CREATE TABLE a.big (id int PRIMARY KEY, b bytea)');

# 100 MB: incompressible (md5s), and compressible (TOAST compresses it,
# as it does in a stock table, then moves it out uncompressed).  (bytea: a
# slice of a text in a multibyte encoding is read from its start.)
sql(q{SET default_toast_compression = pglz;
	ALTER TABLE a.big ALTER COLUMN b SET STORAGE EXTENDED;
	INSERT INTO a.big SELECT 1, convert_to(string_agg(md5(i::text), ''), 'UTF8')
	FROM generate_series(1, 3276800) i});
sql(q{SET default_toast_compression = pglz;
	INSERT INTO a.big SELECT 2, convert_to(repeat('compressible ', 8066000), 'UTF8')});
is(sql(q{SELECT string_agg((test_toast_pointer(b)).stored::text, ',' ORDER BY id) FROM a.big}),
	'104857600,104858000', 'two 100 MB values stored by content, uncompressed');
is(sql(q{CREATE TABLE public.stock (b bytea);
	SET default_toast_compression = pglz;
	INSERT INTO public.stock SELECT b FROM a.big WHERE id = 2;
	SELECT pg_column_compression(b) FROM public.stock}), 'pglz',
	'which a stock table compresses');
sql('DROP TABLE public.stock');

for my $id (1, 2)
{
	my $want = $id == 1 ? q{md5('1562501')} : q{substr(repeat('compressible ', 5), 12, 32)};
	my $out = $node->safe_psql('postgres', qq{
		SELECT count(*) FROM test_toast_counters(true);
		SELECT substr(b, 50000001, 32) = convert_to($want, 'UTF8') FROM a.big WHERE id = $id;
		SELECT nodes_read FROM test_toast_counters(true);});
	my ($ignore, $same, $read) = split /\n/, $out;
	is($same, 't', "value $id: a slice from the middle");
	ok($read > 0 && $read <= 4,
		"value $id: read $read nodes, the slice's and the path to it");
}

# The rest with smaller values, of many nodes still.
sql('TRUNCATE a.big');
sql(q{INSERT INTO a.big SELECT g, convert_to(string_agg(md5(g || '/' || i), ''), 'UTF8')
	FROM generate_series(1, 2) g, generate_series(1, 100000) i GROUP BY g});

my $digest = q{SELECT md5(string_agg(md5(b), ',' ORDER BY id)) FROM a.big};
my $value1 = sql(q{SELECT md5(b) FROM a.big WHERE id = 1});

# A query whose snapshot still sees a deleted row (so VACUUM can't prune
# it) reads its value after a collection swapped the store's files; the
# collection waits for it before removing them.
SKIP:
{
	skip 'injection points not supported by this build', 3
	  if $ENV{enable_injection_points} ne 'yes';
	sql('CREATE EXTENSION injection_points');
	my $reader = $node->background_psql('postgres');
	$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	is($reader->query_safe('SELECT count(*) FROM a.big'), '2', 'a query sees both rows');
	sql('DELETE FROM a.big WHERE id = 1');
	sql('VACUUM a.big');
	my $gc = $node->background_psql('postgres');
	$gc->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-gc-swapped', 'wait')});
	$gc->query_until(qr/collecting/, q{
		\echo collecting
		SELECT test_chunk_gc(false) IS NOT NULL;
		\echo collected
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'prolly-gc-swapped'})
	  or die 'the collection did not swap';
	is($reader->query_safe('SELECT md5(b) FROM a.big WHERE id = 1'), $value1,
		'and reads the deleted row\'s value after a collection swapped');
	sql(q{SELECT injection_points_wakeup('prolly-gc-swapped')});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'virtualxid' AND query LIKE '%test_chunk_gc%'})
	  or die 'the collection did not wait for the reader';
	pass('the collection waits for the query before removing the files');
	$reader->query_safe('COMMIT');
	$reader->quit;
	$gc->query_until(qr/collected/, '');
	$gc->quit;
}

# A standby reads them, through a collection on the primary.
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('acceptance_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;
sql(q{INSERT INTO a.big SELECT 3, convert_to(string_agg(md5(i::text || 's'), ''), 'UTF8')
	FROM generate_series(1, 100000) i});
sql('SELECT test_chunk_gc(false) IS NOT NULL');
my $expected = sql($digest);
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $digest), $expected, 'a standby reads the values');

# A crash keeps them.
sql(q{INSERT INTO a.big SELECT 4, convert_to(string_agg(md5(i::text || 'c'), ''), 'UTF8')
	FROM generate_series(1, 100000) i});
$expected = sql($digest);
$node->stop('immediate');
$node->start;
is(sql($digest), $expected, 'the values survive a crash');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $digest), $expected, 'and the standby has them');

# pg_dump and restore carry them.
my $dump = $node->basedir . '/acceptance.sql';
command_ok([ 'pg_dump', '--file' => $dump, '--dbname' => $node->connstr('postgres') ],
	'pg_dump');
sql('CREATE DATABASE restored');
command_ok([ 'psql', '--no-psqlrc', '--set' => 'ON_ERROR_STOP=1', '--quiet',
	'--file' => $dump, '--dbname' => $node->connstr('restored') ], 'restored');
is($node->safe_psql('restored', $digest), $expected, 'the restored values are the same');
is($node->safe_psql('restored', q{SELECT count(*) FROM a.big
	WHERE test_toast_pointer(b) IS NOT NULL}), '3', 'and stored by content');

$standby->stop;
$node->stop;
done_testing();
