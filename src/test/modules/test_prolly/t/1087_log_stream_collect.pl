# Copyright (c) 2026, PostgreSQL Global Development Group

# dolt_log's scan reads its commits as it gives them, so an open cursor
# over the log reads commits after it began.  A history drop and a full
# collection run meanwhile, in another session, must not free what the
# cursor has yet to read: the scan's start is kept from collection for its
# transaction, so the collection waits for it (or keeps what it reaches),
# and the cursor gives every commit the log held when it began.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('lc');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE lc');

sub sql { return $node->safe_psql('lc', $_[0]); }

sql(q{CREATE SCHEMA lc VERSIONED;
	CREATE TABLE lc.t (id int PRIMARY KEY, v text);
	INSERT INTO lc.t SELECT g, 'v' FROM generate_series(1, 200) g;});
for my $d (1 .. 6)
{
	sql(qq{UPDATE lc.t SET v = 'd$d' WHERE id % 7 = $d;
		SELECT dolt_commit('-Am', 'd$d', '--date', '2020-01-0$d')});
}
my $all = sql(q{SELECT string_agg(message, ',') FROM dolt_log});

# A cursor over the log, two rows read.
my $reader = $node->background_psql('lc', on_error_stop => 0);
$reader->query_safe(q{BEGIN;
	DECLARE c CURSOR FOR SELECT message FROM dolt_log;});
like($reader->query_safe(q{FETCH 2 FROM c}), qr/d6\nd5/, 'two rows read');

# Another session drops the history before the 4th and collects.
my $gc = $node->background_psql('lc', on_error_stop => 0);
my $gc_pid = $gc->query_safe(q{SELECT pg_backend_pid()});
$gc->query_until(qr/started/, q{\echo started
SET client_min_messages = warning;
SELECT count(*) > 0 AS dropped FROM dolt_history_drop('2020-01-04');
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
\echo collected
});
# (until the collection has run, or waits for the reader)
$node->poll_query_until('lc', qq{SELECT state = 'idle' OR wait_event_type = 'Lock'
	FROM pg_stat_activity WHERE pid = $gc_pid})
  or die 'the collection neither ran nor waited';

# The cursor reads the rest: every commit the log held when it began.
my ($rest, $stderr) = ('', '');
$rest = $reader->query(q{FETCH ALL FROM c});
$stderr = $reader->{stderr};
unlike($stderr, qr/missing|ERROR/, 'the rest read without error');
is(join(',', 'd6', 'd5', split(/\n/, $rest)), $all,
	'the cursor gave every commit');
$reader->query_safe(q{COMMIT});
$reader->quit;

like($gc->query_until(qr/collected/, ''), qr/t/, 'the collection ran');
$gc->quit;

# Collected, the history is dropped.
cmp_ok(sql(q{SET client_min_messages = warning;
	SELECT count(*) FROM dolt_log}), '<',
	scalar(split(/,/, $all)), 'the history dropped');

# The horizon a cursor stops at is the one it began with: a history drop
# committed meanwhile in another session, and a rollback to a savepoint in
# the cursor's transaction (which reads the horizon again), don't cut the
# rest short.
for my $d (7 .. 8)
{
	sql(qq{UPDATE lc.t SET v = 'd$d' WHERE id % 7 = 1;
		SELECT dolt_commit('-Am', 'd$d', '--date', '2020-01-0$d')});
}
my $held = sql(q{SET client_min_messages = warning;
	SELECT string_agg(message, ',') FROM dolt_log});
$reader = $node->background_psql('lc', on_error_stop => 0);
$reader->query_safe(q{BEGIN;
	DECLARE c CURSOR FOR SELECT message FROM dolt_log;});
like($reader->query_safe(q{FETCH 2 FROM c}), qr/d8\nd7/, 'two rows read again');
sql(q{SET client_min_messages = warning;
	SELECT count(*) > 0 FROM dolt_history_drop('2020-01-06')});
$reader->query_safe(q{SAVEPOINT s; SELECT 1; ROLLBACK TO SAVEPOINT s;});
$rest = $reader->query_safe(q{SET client_min_messages = warning; FETCH ALL FROM c});
is(join(',', 'd8', 'd7', split(/\n/, $rest)), $held,
	'the cursor stops at the horizon it began with');
$reader->query_safe(q{COMMIT});
$reader->quit;

done_testing();
