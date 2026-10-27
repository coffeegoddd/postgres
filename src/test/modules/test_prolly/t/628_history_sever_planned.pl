# Copyright (c) 2026, PostgreSQL Global Development Group

# A sever plans, and archives, holding no ref: a commit while it does
# doesn't wait for it.  The refs moved meanwhile, it plans (and archives)
# again, still holding none; then, holding them, it only checks the plan
# and records it, so what the commit made is kept.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('sp');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE sp');
sub sql { return $node->safe_psql('sp', $_[0]); }

sql(q{CREATE EXTENSION injection_points});
sql(q{CREATE SCHEMA sp VERSIONED;
	CREATE TABLE sp.t (id int PRIMARY KEY, v text);
	INSERT INTO sp.t SELECT g, 'v' FROM generate_series(1, 100) g;});
for my $d (1 .. 4)
{
	sql(qq{UPDATE sp.t SET v = 'd$d' WHERE id % 5 = $d;
		SELECT dolt_commit('-Am', 'd$d', '--date', '2020-01-0$d')});
}

my $sever = $node->background_psql('sp', on_error_stop => 0);
$sever->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-sever-planned', 'wait')});
$sever->query_until(qr/started/, q{\echo started
SELECT commits FROM dolt_history_drop('2020-01-04');
\echo severed
});
$node->poll_query_until('sp', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-sever-planned'})
  or die 'the sever did not stop after planning';

# A branch made at the oldest commit, and a commit on it, while the
# sever holds no ref: neither waits.
my $c = sql(q{SET statement_timeout = '10s';
	SELECT dolt_branch('side', 'HEAD~3');
	SELECT dolt_checkout('side');
	UPDATE sp.t SET v = 'side' WHERE id = 1;
	SELECT trim(both '{}' from dolt_commit('-am', 'side now')::text)});
$c = (split /\n/, $c)[-1];
like($c, qr/^[0-9a-f]{40}$/, 'a commit while the sever plans goes through');

sql(q{SELECT injection_points_wakeup('prolly-sever-planned')});
$sever->query_until(qr/severed/, '');
$sever->quit;

# The refs moved: planned again, with side, whose merge base with main
# is the oldest commit, so nothing is severed.
is(sql(q{SELECT count(*) FROM prolly_history_horizon()}), '0',
	'planned again: the new branch\'s base is kept');
is(sql(q{SET client_min_messages = warning;
	SELECT count(*) FROM dolt_log('side')}), '2',
	'side keeps its history');

# Archived: a commit on main while the sever is between its archive and
# its locks doesn't wait for it either.  The refs moved, so the sever
# plans and archives again, holding nothing still, and then records the
# fragment, the same one (its names in the archive by its content).
my $arch = PostgreSQL::Test::Utils::tempdir() . '/archive';
sql(qq{SELECT dolt_branch('-D', 'side');
	ALTER DATABASE sp SET prolly.history_window = '1 day';
	ALTER DATABASE sp SET prolly.history_archive = 'file://$arch'});
$sever = $node->background_psql('sp', on_error_stop => 0);
$sever->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-sever-planned', 'wait')});
$sever->query_until(qr/started/, q{\echo started
SELECT fragment > 0 AS archived FROM dolt_history_sever();
\echo severed
});
$node->poll_query_until('sp', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-sever-planned'})
  or die 'the sever did not stop after archiving';
$c = sql(q{SET statement_timeout = '10s';
	UPDATE sp.t SET v = 'main now' WHERE id = 2;
	SELECT trim(both '{}' from dolt_commit('-am', 'main now')::text)});
$c = (split /\n/, $c)[-1];
like($c, qr/^[0-9a-f]{40}$/, 'a commit while the sever archives goes through');
# (woken, it stops there again after archiving again; woken again, on)
sql(q{SELECT injection_points_wakeup('prolly-sever-planned')});
$node->poll_query_until('sp', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-sever-planned'})
  or die 'the sever did not plan and archive again';
pass('the refs moved: planned and archived again, holding nothing');
for (1 .. 20)
{
	last if sql(q{SELECT count(*) FROM pg_stat_activity
		WHERE wait_event = 'prolly-sever-planned'}) eq '0';
	sql(q{SELECT injection_points_wakeup('prolly-sever-planned')});
	sleep(1);
}
$sever->query_until(qr/severed/, '');
$sever->quit;
is(sql(q{SELECT count(*) FROM prolly_history_horizon() WHERE state = 'archived'}), '1',
	'archived and severed, planned again past the commit');
is(sql(q{SET client_min_messages = warning;
	SELECT count(*) FROM dolt_log('main') WHERE message = 'main now'}), '1',
	'the commit is kept');

# Commits all the while, a window of two seconds and no batch: the sever's
# cut is fixed when it begins, so each plan it makes again takes in only
# the commits made since, above the cut, and the fragment it archived is
# the one it records.  (A slow archive, a tenth of a second and more a
# chunk, so commits land while it sends.)
$node->safe_psql('postgres', 'CREATE DATABASE cl');
sub csql { return $node->safe_psql('cl', $_[0]); }
csql(q{CREATE SCHEMA cl VERSIONED;
	CREATE TABLE cl.t (id int PRIMARY KEY, v text);
	INSERT INTO cl.t SELECT g, 'v' FROM generate_series(1, 200) g;});
for my $d (1 .. 6)
{
	csql(qq{UPDATE cl.t SET v = 'd$d' WHERE id % 7 = $d;
		SELECT dolt_commit('-Am', 'd$d', '--date', '2020-01-0$d')});
}
my $slow = 'slow://' . PostgreSQL::Test::Utils::tempdir() . '/slow';
csql(qq{ALTER DATABASE cl SET prolly.history_window = '2 seconds';
	ALTER DATABASE cl SET prolly.history_archive = '$slow'});
my $bg = $node->background_psql('cl', on_error_stop => 0);
$bg->query_until(qr/started/, q{\echo started
LOAD 'test_prolly';
SELECT fragment > 0 AS archived FROM dolt_history_sever();
\echo severed
});
my $made = 0;
for my $i (1 .. 40)
{
	csql(qq{SET lock_timeout = '5s';
		UPDATE cl.t SET v = 'now $i' WHERE id = 1;
		SELECT dolt_commit('-am', 'now $i')});
	$made++;
	last if csql(q{SELECT count(*) FROM pg_stat_activity
		WHERE datname = 'cl' AND query LIKE '%dolt_history_sever%'
		AND state = 'active'}) eq '0';
}
my $out = $bg->query_until(qr/severed/, '');
$bg->quit;
cmp_ok($made, '>', 1, 'commits went on while the sever ran');
is(csql(q{SELECT count(*) FROM prolly_history_horizon()}), '1',
	'severed while commits kept arriving: the cut fixed when it began');

done_testing();
