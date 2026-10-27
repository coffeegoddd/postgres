# Copyright (c) 2026, PostgreSQL Global Development Group

# A repair and a full collection take the history horizon's lock before
# anything else (the refs, the chunk store's collection), so the two wait
# for one another, never deadlock: a collection begun while a repair holds
# the horizon waits for it to commit, then collects, keeping what it
# repaired.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('rc');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE rc');
my $url = 'file://' . PostgreSQL::Test::Utils::tempdir() . '/remote';

sub sql { return $node->safe_psql('rc', $_[0]); }

sql(q{CREATE SCHEMA rc VERSIONED;
	CREATE TABLE rc.t (id int PRIMARY KEY, v text);
	INSERT INTO rc.t SELECT g, 'v' FROM generate_series(1, 200) g;});
for my $d (1 .. 6)
{
	sql(qq{UPDATE rc.t SET v = 'd$d' WHERE id % 7 = $d;
		SELECT dolt_commit('-Am', 'd$d', '--date', '2020-01-0$d')});
}
sql(qq{SELECT dolt_remote('add', 'origin', '$url');
	SELECT dolt_push('origin', 'main')});
sql(q{SELECT commits FROM dolt_history_drop('2020-01-04')});
sql(q{SELECT * FROM prolly_gc(true)});
my $logged = q{SET client_min_messages = warning;
	SELECT count(*) FROM dolt_log('main')};
my $short = sql($logged);

my $repair = $node->background_psql('rc');
$repair->query_safe('BEGIN');
$repair->query_safe(q{SELECT dolt_history_repair('origin')});

# The collection waits for the repair's horizon lock.
my $gc = $node->background_psql('rc');
$gc->query_until(qr/started/, q{\echo started
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
\echo collected
});
$node->poll_query_until('rc', q{SELECT count(*) > 0 FROM pg_locks
	WHERE NOT granted AND locktype = 'object'})
  or die 'the collection did not wait';
$repair->query_safe('COMMIT');
$repair->quit;
like($gc->query_until(qr/collected/, ''), qr/collected/, 'the collection ran after');
$gc->quit;

cmp_ok(sql($logged), '>', $short, 'the history repaired is kept');
ok(!$node->log_contains(qr/deadlock detected/), 'no deadlock');

done_testing();
