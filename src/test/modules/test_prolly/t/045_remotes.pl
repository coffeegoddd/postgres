# Copyright (c) 2026, PostgreSQL Global Development Group

# Remotes across failures: a fetch that fails before the store names what
# it received leaves nothing found and nothing tracked, and the next one
# succeeds; a fetch survives a crash; a standby reads what was fetched; a
# push that fails before the remote names what it sent leaves the remote
# as it was, and the next one succeeds; a remote of more table files than
# a backend may hold open is read.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('remotes');
$node->init(allows_streaming => 1);
# (few open files allowed: a remote of more table files than that is read)
$node->append_conf('postgresql.conf', qq{autovacuum = off
max_files_per_process = 64});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
$node->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('remotes_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

my $url = 'file://' . PostgreSQL::Test::Utils::tempdir() . '/origin';

sub sql { return $node->safe_psql($_[0], $_[1]); }

# a database with history, pushed
sql('postgres', qq{
	CREATE SCHEMA r VERSIONED;
	CREATE TABLE r.t (id int PRIMARY KEY, v text);
	INSERT INTO r.t SELECT g, 'v' || g FROM generate_series(1, 300) g;
	SELECT dolt_commit('-Am', 'one');
	UPDATE r.t SET v = 'w' WHERE id <= 10;
	SELECT dolt_commit('-am', 'two');
	SELECT dolt_remote('add', 'origin', '$url');
	SELECT dolt_push('origin', 'main');
});
my $head = sql('postgres', q{SELECT dolt_hashof('main')});

sql('postgres', 'CREATE DATABASE b');
sql('b', qq{SELECT dolt_remote('add', 'origin', '$url')});
my $tracked = q{SELECT count(*) FROM dolt_remote_branches};
my $has_head = qq{SELECT test_chunk_has('$head')};
my $files = q{SELECT count(*) FROM test_chunk_table_files()};

SKIP:
{
	skip 'injection points not supported by this build', 4
	  if $ENV{enable_injection_points} ne 'yes';

	# A fetch that fails after writing its table file, before naming it.
	sql('b', 'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
	sql('b', q{SELECT injection_points_attach('prolly-table-file-written', 'error')});
	my ($ret, $out, $err) = $node->psql('b', 'SELECT dolt_fetch()');
	like($err, qr/error triggered for injection point prolly-table-file-written/,
		'a fetch fails after its table file, before naming it');
	sql('b', q{SELECT injection_points_detach('prolly-table-file-written')});
	is(sql('b', $tracked), '0', 'nothing is tracked');
	is(sql('b', $has_head), 'f', 'nor found');
	is(sql('b', $files), '0', 'nor any table file named');
}

sql('b', 'CREATE EXTENSION IF NOT EXISTS test_prolly');
sql('b', 'SELECT dolt_fetch()');
is(sql('b', q{SELECT dolt_hashof('origin/main')}), $head, 'the next fetch succeeds');

# A fetch survives a crash.
$node->stop('immediate');
$node->start;
is(sql('b', q{SELECT dolt_hashof('origin/main')}), $head, 'a fetch survives a crash');
is(sql('b', q{SELECT count(*) FROM dolt_log('origin/main')}), '2',
	'its commits read back');
# (a checkout is the session's: each call checks out again)
sql('b', q{SELECT dolt_branch('work', 'origin/main')});
is(sql('b', q{SELECT dolt_checkout('work') IS NULL;
	SELECT count(*) FILTER (WHERE v = 'w') FROM r.t}), "f\n10", 'and its rows');

# A standby reads what was fetched.
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('b', q{SELECT dolt_hashof('origin/main')}), $head,
	'a standby has the remote-tracking branch');
is($standby->safe_psql('b', q{SELECT count(*) FROM dolt_log('origin/main')}), '2',
	'and reads its commits');

SKIP:
{
	skip 'injection points not supported by this build', 3
	  if $ENV{enable_injection_points} ne 'yes';

	# A push that fails after writing the remote's table file, before
	# naming it: the remote is as it was.
	sql('b', q{SELECT dolt_checkout('work'); UPDATE r.t SET v = 'b' WHERE id = 1;
		SELECT dolt_commit('-am', 'from b')});
	sql('b', q{SELECT injection_points_attach('prolly-remote-file-written', 'error')});
	my ($ret, $out, $err) = $node->psql('b', q{SELECT dolt_push('origin', 'work')});
	like($err, qr/error triggered for injection point prolly-remote-file-written/,
		'a push fails after the remote\'s file, before naming it');
	sql('b', q{SELECT injection_points_detach('prolly-remote-file-written')});
	sql('postgres', 'SELECT dolt_fetch()');
	is(sql('postgres', q{SELECT count(*) FROM dolt_remote_branches WHERE name LIKE '%/work'}),
		'0', 'the remote hasn\'t the branch');
	sql('b', q{SELECT dolt_push('origin', 'work')});
	sql('postgres', q{SELECT dolt_fetch()});
	is(sql('postgres', q{SELECT dolt_hashof('origin/work')}),
		sql('b', q{SELECT dolt_hashof('work')}), 'the next push succeeds');
}

# A remote of more table files than a backend may hold open: each push
# adds one, and a fetch from a database that has none of them reads them
# all.
sql('postgres', join("\n", map { qq{UPDATE r.t SET v = 'p$_' WHERE id = 300;
	SELECT dolt_commit('-am', 'push $_'); SELECT dolt_push('origin', 'main');} } 1 .. 40));
sql('postgres', 'CREATE DATABASE c');
sql('c', qq{SELECT dolt_remote('add', 'origin', '$url'); SELECT dolt_fetch('origin', 'main')});
is(sql('c', q{SELECT dolt_hashof('origin/main')}), sql('postgres', q{SELECT dolt_hashof('main')}),
	'a remote of more table files than may be open is read');

done_testing();
