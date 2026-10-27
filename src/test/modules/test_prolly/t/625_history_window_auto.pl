# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum severs the history past the database's window, as the
# database's settings have it (ALTER DATABASE ... SET), never a session's:
# a non-owner's SET prolly.history_window_action = delete, then commits
# past the window, deletes nothing.  With the database's action delete, a
# visit severs and a full collection frees what only the severed history
# reached.  Over prolly.history_max_size, each visit severs a batch more,
# each branch still keeping its newest prolly.history_min_commits.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('auto');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE hw');

sub sql { return $node->safe_psql('hw', $_[0]); }

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
	$node->poll_query_until('hw', "SELECT ($count) > $before")
	  or die 'autovacuum did not visit';
	$node->poll_query_until('postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker' AND datname = 'hw'})
	  or die 'the autovacuum worker did not leave';
}

# History: a commit a day for ten days.
sql(q{CREATE SCHEMA hw VERSIONED;
	CREATE TABLE hw.t (id int PRIMARY KEY, v text);
	INSERT INTO hw.t SELECT g, 'v' FROM generate_series(1, 300) g;});
for my $ago (reverse 1 .. 10)
{
	sql(qq{UPDATE hw.t SET v = 'd$ago' WHERE id % 11 = $ago;
		SELECT dolt_commit('-Am', 'd$ago', '--date',
			(now() - interval '$ago days')::text)});
}
my $logged = q{SELECT count(*) FROM dolt_log('main')};
# (the log's notice that it stops at the horizon kept out of a poll's stderr)
$node->safe_psql('hw', q{ALTER DATABASE hw SET client_min_messages = warning});
is(sql($logged), '10', 'ten commits');

# A window of three days, archived (the default) with no archive: nothing
# severed, though a non-owner's session says delete and commits past it.
sql(q{ALTER DATABASE hw SET prolly.history_warm = '3 days';
	ALTER DATABASE hw SET prolly.history_cold_batch = '1 second';
	CREATE ROLE other LOGIN;
	GRANT ALL ON SCHEMA hw TO other;
	GRANT ALL ON hw.t TO other;
	GRANT pg_prolly_write TO other;});
$node->safe_psql('hw', q{SET prolly.history_window_action = delete;
	UPDATE hw.t SET v = 'other' WHERE id = 1;
	SELECT dolt_commit('-am', 'other''s', '--date', (now() - interval '20 days')::text);},
	extra_params => [ '-U', 'other' ]);
autovacuum_visited();
autovacuum_visited();
is(sql(q{SELECT count(*) FROM prolly_history_horizon()}), '0',
	'a session\'s delete severs nothing');
ok($node->log_contains(qr/is past its window, but isn't severed: it has no archive/),
	'archive mode with no archive says so');

# The database's delete: a visit severs, and its collection frees.
sql(q{ALTER DATABASE hw SET prolly.history_window_action = delete});
$node->poll_query_until('hw', q{SELECT count(*) > 0 FROM prolly_history_horizon()})
  or die 'nothing severed';
autovacuum_visited();
my $top = sql(q{SELECT tops[1] FROM prolly_history_horizon() LIMIT 1});
my ($ret, $out, $err) = $node->psql('hw', qq{SELECT prolly_root_hash('$top')});
like($err, qr/is past the history horizon/, 'the severed commit is freed');
cmp_ok(sql($logged), '<', 11, 'the log is shorter');
like($node->safe_psql('hw', q{SELECT state FROM prolly_history_horizon() LIMIT 1}),
	qr/deleted/, 'deleted');

# Over the size limit: a batch more each visit, down to the newest three.
sql(q{ALTER DATABASE hw SET prolly.history_warm = '';
	ALTER DATABASE hw SET prolly.history_min_commits = 3;
	ALTER DATABASE hw SET prolly.history_max_size = '1kB';});
$node->poll_query_until('hw', qq{SELECT ($logged) <= 3})
  or die 'the size limit severed nothing';
autovacuum_visited();
autovacuum_visited();
cmp_ok(sql($logged), '>=', 3, 'the newest three are kept, however large');

done_testing();
