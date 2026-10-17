# Copyright (c) 2026, PostgreSQL Global Development Group

# Remotes across failures: a fetch that fails, or crashes, before the
# store names what it received leaves nothing found and nothing tracked,
# and the next one succeeds, writing over what the failed one left; a
# committed fetch survives a crash; a standby reads what was fetched, rows
# too; a push that fails before the remote names what it sent leaves the
# remote as it was, byte for byte; a remote of more table files than a
# backend may hold open is read.

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
sql('b', qq{CREATE EXTENSION test_prolly; SELECT dolt_remote('add', 'origin', '$url')});
my $tracked = q{SELECT count(*) FROM dolt_remote_branches};
my $has_head = qq{SELECT test_chunk_has('$head')};
my $named = q{SELECT coalesce(string_agg(name, ' ' ORDER BY name), '') FROM test_chunk_table_files()};

# a database's table files on disk, named or not
sub on_disk
{
	return sql($_[0], q{SELECT coalesce(string_agg(f, ' ' ORDER BY f), '')
		FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = current_database()) db,
			pg_ls_dir(d) f
		WHERE f LIKE 'prolly\_table\_%'});
}

# the remote's files, each with its contents' md5
my $remote_dir = $url =~ s{^file://}{}r;
sub remote_files
{
	opendir(my $dh, $remote_dir) or die "could not open $remote_dir: $!";
	my @f = sort grep { !/^\./ } readdir($dh);
	closedir($dh);
	return join(' ', map {
		my $path = "$remote_dir/$_";
		open(my $fh, '<', $path) or die "could not open $path: $!";
		binmode($fh);
		local $/;
		my $bytes = <$fh>;
		close($fh);
		"$_:" . length($bytes) . ':' . unpack('%32C*', $bytes)
	} @f);
}

SKIP:
{
	skip 'injection points not supported by this build', 6
	  if $ENV{enable_injection_points} ne 'yes';

	# A fetch that fails after writing its table file, before naming it:
	# the file stays unnamed, and the next fetch writes over it.  (The
	# injection point is this session's alone.)
	sql('b', 'CREATE EXTENSION injection_points');
	my ($ret, $out, $err) = $node->psql('b', q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-table-file-written', 'error');
		SELECT dolt_fetch()});
	like($err, qr/error triggered for injection point prolly-table-file-written/,
		'a fetch fails after its table file, before naming it');
	is(sql('b', $tracked), '0', 'nothing is tracked');
	is(sql('b', $has_head), 'f', 'nor found');
	is(sql('b', $named), '', 'nor any table file named');
	my $left = on_disk('b');
	isnt($left, '', 'its file stays on disk, unnamed');
	sql('b', 'SELECT dolt_fetch()');
	is(sql('b', $named), $left, 'the next fetch writes over it, and names it');
}

sql('b', 'SELECT dolt_fetch()');
is(sql('b', q{SELECT dolt_hashof('origin/main')}), $head, 'a fetch succeeds');

SKIP:
{
	skip 'injection points not supported by this build', 4
	  if $ENV{enable_injection_points} ne 'yes';

	# A crash while a fetch is between its table file and its manifest.
	sql('postgres', 'CREATE DATABASE d');
	sql('d', qq{CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
		SELECT dolt_remote('add', 'origin', '$url')});
	my $f = $node->background_psql('d', on_error_stop => 0);
	$f->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-table-file-written', 'wait')});
	$f->query_until(qr/started/, q{
		\echo started
		SELECT dolt_fetch();
	});
	$node->poll_query_until('d', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'prolly-table-file-written'}) or die 'the fetch did not wait';
	$node->stop('immediate');
	eval { $f->quit; };
	$node->start;
	is(sql('d', $tracked), '0', 'a fetch a crash stopped before naming its file tracks nothing');
	is(sql('d', $named), '', 'and names no table file');
	sql('d', 'SELECT dolt_fetch()');
	is(sql('d', q{SELECT dolt_hashof('origin/main')}), $head, 'the next fetch succeeds');
	is(sql('d', q{SELECT count(*) FROM dolt_log('origin/main')}), '2',
		'and its commits read back');
}

# A committed fetch survives a crash.
$node->stop('immediate');
$node->start;
is(sql('b', q{SELECT dolt_hashof('origin/main')}), $head, 'a committed fetch survives a crash');
is(sql('b', q{SELECT count(*) FROM dolt_log('origin/main')}), '2',
	'its commits read back');
# (a connection to a branch with only a head, whose copies take DDL: they
# were made at startup, which crashed the backend)
sql('b', q{SELECT dolt_branch('work', 'origin/main')});
is(sql('b/work', q{SELECT count(*) FILTER (WHERE v = 'w') FROM r.t}), '10', 'and its rows');

# A standby reads what was fetched: the refs, the commits, and the trees of
# their rows (a diff of the two commits reads both).
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('b', q{SELECT dolt_hashof('origin/main')}), $head,
	'a standby has the remote-tracking branch');
is($standby->safe_psql('b', q{SELECT count(*) FROM dolt_log('origin/main')}), '2',
	'and reads its commits');
is($standby->safe_psql('b/work', q{SELECT count(*)
	FROM dolt_diff('origin/main~1', 'origin/main', NULL::r.t)}), '10',
	'and their rows');

SKIP:
{
	skip 'injection points not supported by this build', 4
	  if $ENV{enable_injection_points} ne 'yes';

	# A push that fails after writing the remote's table file, before
	# naming it: the remote is as it was, byte for byte, its file removed.
	sql('b/work', q{UPDATE r.t SET v = 'b' WHERE id = 1; SELECT dolt_commit('-am', 'from b')});
	my $before = remote_files();
	my ($ret, $out, $err) = $node->psql('b', q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-remote-file-written', 'error');
		SELECT dolt_push('origin', 'work')});
	like($err, qr/error triggered for injection point prolly-remote-file-written/,
		'a push fails after the remote\'s file, before naming it');
	is(remote_files(), $before, 'the remote is as it was, its file removed');
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
