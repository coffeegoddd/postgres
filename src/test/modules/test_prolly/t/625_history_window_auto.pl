# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum severs the history past the database's window, as the
# database's settings have it (ALTER DATABASE ... SET), never a session's:
# a non-owner's SET prolly.history_window_action = delete, then commits
# past the window, deletes nothing.  With the database's action delete, a
# visit severs and a full collection frees what only the severed history
# reached.  Over prolly.history_max_size, as the last full collection
# measured the store, a batch more for each collection measured, each
# branch still keeping its newest prolly.history_min_commits; under it,
# nothing.  It runs as the database's owner, whose archive's checks are
# the owner's; a failure is retried after a wait, not at every visit.  A
# branch's own window is severed with none for the database.

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
sql(q{ALTER DATABASE hw SET prolly.history_window = '3 days';
	ALTER DATABASE hw SET prolly.history_batch = '1 second';
	ALTER DATABASE hw SET prolly.history_collect_interval = '1 second';
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

# Under the size limit, as measured: nothing more severed.
sql(q{ALTER DATABASE hw SET prolly.history_window = '';
	ALTER DATABASE hw SET prolly.history_min_commits = 3;
	ALTER DATABASE hw SET prolly.history_batch = '1 day';
	ALTER DATABASE hw SET prolly.history_max_size = '1GB';});
my $fragments = q{SELECT count(*) FROM prolly_history_horizon()};
my $before = sql($fragments);
autovacuum_visited();
autovacuum_visited();
autovacuum_visited();
is(sql($fragments), $before, 'under the size limit, nothing more is severed');

# Over it: a batch more for each collection measured, down to the newest
# three (delete mode: two measurements over in a row for each).
sql(q{ALTER DATABASE hw SET prolly.history_max_size = '1kB'});
$node->poll_query_until('hw', qq{SELECT ($logged) <= 3})
  or die 'the size limit severed nothing';
autovacuum_visited();
autovacuum_visited();
cmp_ok(sql($logged), '>=', 3, 'the newest three are kept, however large');
sql(q{ALTER DATABASE hw RESET prolly.history_max_size;
	ALTER DATABASE hw RESET prolly.history_min_commits});

# A branch's own window, none for the database's: severed in the
# background too, by the branch's.
$node->safe_psql('postgres', 'CREATE DATABASE hb');
sub bsql { return $node->safe_psql('hb', $_[0]); }
bsql(q{CREATE TABLE visits (n int) USING heap WITH
	(autovacuum_vacuum_insert_threshold = 0,
	 autovacuum_vacuum_insert_scale_factor = 0);
	CREATE SCHEMA hb VERSIONED;
	CREATE TABLE hb.t (id int PRIMARY KEY, v text);
	INSERT INTO hb.t SELECT g, 'v' FROM generate_series(1, 100) g;});
for my $ago (reverse 1 .. 5)
{
	bsql(qq{UPDATE hb.t SET v = 'd$ago' WHERE id = $ago;
		SELECT dolt_commit('-Am', 'd$ago', '--date',
			(now() - interval '$ago days')::text)});
}
bsql(q{ALTER DATABASE hb SET prolly.history_window_action = delete;
	ALTER DATABASE hb SET prolly.history_batch = '1 second';
	ALTER DATABASE hb SET prolly.history_collect_interval = '1 second';
	SELECT dolt_history_window('main', 'window', '2 days');});
$node->poll_query_until('hb', q{SELECT count(*) > 0 FROM prolly_history_horizon()})
  or die 'the branch\'s window severed nothing';
pass('a branch\'s own window is severed in the background');

# Over the size limit with history whose dates can't move the cut: a
# commit dated 2000 above commits dated now.  The size limit's cut, a batch
# past the oldest date, cuts nothing by date (every commit beneath the old
# one is new), so it cuts by height instead, and the limit still severs.
$node->safe_psql('postgres', 'CREATE DATABASE hs');
sub ssql { return $node->safe_psql('hs', $_[0]); }
ssql(q{CREATE TABLE visits (n int) USING heap WITH
	(autovacuum_vacuum_insert_threshold = 0,
	 autovacuum_vacuum_insert_scale_factor = 0);
	CREATE SCHEMA hs VERSIONED;
	CREATE TABLE hs.t (id int PRIMARY KEY, v text);
	INSERT INTO hs.t SELECT g, 'v' FROM generate_series(1, 100) g;});
for my $i (1 .. 6)
{
	ssql(qq{UPDATE hs.t SET v = 'n$i' WHERE id = $i;
		SELECT dolt_commit('-Am', 'n$i')});
}
ssql(q{UPDATE hs.t SET v = 'old' WHERE id = 7;
	SELECT dolt_commit('-am', 'old', '--date', '2000-01-01');
	UPDATE hs.t SET v = 'top' WHERE id = 8;
	SELECT dolt_commit('-am', 'top');
	ALTER DATABASE hs SET client_min_messages = warning;
	ALTER DATABASE hs SET prolly.history_window_action = delete;
	ALTER DATABASE hs SET prolly.history_batch = '1 day';
	ALTER DATABASE hs SET prolly.history_collect_interval = '1 second';
	ALTER DATABASE hs SET prolly.history_max_size = '1kB';});
my $severed = 0;
for (1 .. 60)
{
	$severed = ssql(q{SELECT count(*) FROM prolly_history_horizon()});
	last if $severed ne '0';
	ssql('INSERT INTO visits VALUES (1)');
	select(undef, undef, undef, 0.5);
}
isnt($severed, '0', 'a size limit whose dates cut nothing cuts by height');
is(ssql(q{SELECT count(*) FROM dolt_log('main') WHERE message IN ('old', 'top')}),
	'2', 'the newest are kept');

# As the database's owner: one not a superuser, whose archive is a file
# URL, isn't let write the server's files, as dolt_history_sever() isn't.
# The failure is reported once, then waited out, not tried each visit.
my $arch = PostgreSQL::Test::Utils::tempdir() . '/owned';
$node->safe_psql('postgres', q{CREATE ROLE owner LOGIN CREATEDB});
$node->safe_psql('postgres', q{CREATE DATABASE ho}, extra_params => [ '-U', 'owner' ]);
sub osql { return $node->safe_psql('ho', $_[0], extra_params => [ '-U', 'owner' ]); }
osql(q{CREATE SCHEMA ho VERSIONED;
	CREATE TABLE ho.t (id int PRIMARY KEY, v text);
	INSERT INTO ho.t SELECT g, 'v' FROM generate_series(1, 100) g;});
for my $ago (reverse 1 .. 5)
{
	osql(qq{UPDATE ho.t SET v = 'd$ago' WHERE id = $ago;
		SELECT dolt_commit('-Am', 'd$ago', '--date',
			(now() - interval '$ago days')::text)});
}
my $logpos = -s $node->logfile;
osql(qq{ALTER DATABASE ho SET prolly.history_window = '2 days';
	ALTER DATABASE ho SET prolly.history_batch = '1 second';
	ALTER DATABASE ho SET prolly.history_archive = 'file://$arch'});
$node->wait_for_log(qr/background severing of history of database "ho" failed: permission denied/, $logpos);
ok(!-e $arch, 'the owner\'s archive in the server\'s files isn\'t made');
is($node->safe_psql('ho', q{SELECT count(*) FROM prolly_history_horizon()}), '0',
	'nothing is severed');
# (visits meanwhile wait the failure out: still the one report)
$node->safe_psql('ho', q{CREATE TABLE visits (n int) USING heap WITH
	(autovacuum_vacuum_insert_threshold = 0,
	 autovacuum_vacuum_insert_scale_factor = 0)});
for (1 .. 3)
{
	my $count = q{SELECT coalesce(autovacuum_count, 0) FROM pg_stat_user_tables
		WHERE relname = 'visits'};
	my $b = $node->safe_psql('ho', $count);
	$node->safe_psql('ho', 'INSERT INTO visits VALUES (1)');
	$node->poll_query_until('ho', "SELECT ($count) > $b") or die 'no visit';
}
my $log = slurp_file($node->logfile, $logpos);
my $reports = () = $log =~ /background severing of history of database "ho" failed/g;
is($reports, 1, 'a failure is retried after a wait, not every visit');

done_testing();
