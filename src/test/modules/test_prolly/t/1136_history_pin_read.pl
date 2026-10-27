# Copyright (c) 2026, PostgreSQL Global Development Group

# A reader's pin keeps its commit's data across a sever and a full
# collection made while it reads: a snapshot mount of a commit past the
# window, a cursor over it with a row read, then another session severs
# (delete mode) and collects, and the cursor reads the rest.  Only the
# history behind the pinned commit is severed.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pr');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE pr');
sub sql { return $node->safe_psql('pr', $_[0]); }

sql(q{CREATE SCHEMA pr VERSIONED;
	CREATE TABLE pr.t (id int PRIMARY KEY, v text);
	INSERT INTO pr.t SELECT g, 'p' FROM generate_series(1, 500) g;
	SELECT dolt_commit('-Am', 'p10', '--date', (now() - interval '10 days')::text);});
my $pinned = sql(q{SELECT dolt_hashof('HEAD')});
sql(qq{SELECT prolly_mount('$pinned', 'pr', 'old')});
for my $d (reverse 4 .. 9)
{
	sql(qq{UPDATE pr.t SET v = 'd$d';
		SELECT dolt_commit('-am', 'd$d', '--date', (now() - interval '$d days')::text)});
}
sql(q{UPDATE pr.t SET v = 'now';
	SELECT dolt_commit('-am', 'now');
	ALTER DATABASE pr SET prolly.history_window = '3 days';
	ALTER DATABASE pr SET prolly.history_window_action = delete;});

my $reader = $node->background_psql('pr', on_error_stop => 0);
$reader->query_safe(q{BEGIN;
	DECLARE c CURSOR FOR SELECT id, v FROM old.t ORDER BY id;});
like($reader->query_safe(q{FETCH 1 FROM c}), qr/^1\|p$/, 'a row read');

sql(q{SELECT fragment FROM dolt_history_sever()});
is(sql(q{SELECT sum(commits) FROM prolly_history_horizon()}), '6',
	'the history behind the pinned commit is severed');
# (the collection in a session of its own: it waits for the reader, or
# keeps what the reader reaches)
my $gc = $node->background_psql('pr', on_error_stop => 0);
my $gc_pid = $gc->query_safe(q{SELECT pg_backend_pid()});
$gc->query_until(qr/started/, q{\echo started
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
\echo collected
});
$node->poll_query_until('pr', qq{SELECT state = 'idle' OR wait_event_type = 'Lock'
	FROM pg_stat_activity WHERE pid = $gc_pid})
  or die 'the collection neither ran nor waited';

my $rest = $reader->query(q{FETCH ALL FROM c});
unlike($reader->{stderr}, qr/ERROR|missing|horizon/, 'the rest read without error');
is(scalar(split(/\n/, $rest)), 499, 'every row read');
$reader->query_safe(q{COMMIT});
$reader->quit;
like($gc->query_until(qr/collected/, ''), qr/t/, 'the collection ran');
$gc->quit;

# Read again, in a new transaction: the pin keeps the data.
is(sql(q{SELECT count(*) FROM old.t WHERE v = 'p'}), '500',
	'the pinned commit\'s data is kept');
my ($ret, $out, $err) = $node->psql('pr', q{SELECT count(*) FROM
	dolt_at((SELECT commit_hash FROM dolt_log WHERE message = 'd9'), NULL::pr.t)});
like($err, qr/is past the history horizon/, 'what is behind it is severed');

done_testing();
