# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_upgrade carries over only the relations users made, not the history
# store initdb makes: a cluster whose store holds chunks or pointers is
# refused, and one whose stores are empty is not.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $old = PostgreSQL::Test::Cluster->new('history_old');
my $new = PostgreSQL::Test::Cluster->new('history_new');
$old->init;
$new->init;

my $bindir = $new->config_data('--bindir');
my @check = (
	'pg_upgrade', '--no-sync', '--check',
	'--old-datadir' => $old->data_dir,
	'--new-datadir' => $new->data_dir,
	'--old-bindir' => $bindir,
	'--new-bindir' => $bindir,
	'--socketdir' => $new->host,
	'--old-port' => $old->port,
	'--new-port' => $new->port);

# (pg_upgrade writes its files in the directory it runs in)
chdir PostgreSQL::Test::Utils::tempdir;

command_ok(\@check, 'a cluster whose history stores are empty may be upgraded');

my $refused = sub {
	my ($name) = @_;
	my ($stdout, $stderr) = run_command(\@check);
	like($stdout . $stderr,
		qr/Database "postgres" has version-control history, which cannot be upgraded yet/, $name);
};

$old->start;
$old->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	SELECT test_chunk_put('pg_history.chunks', 'pg_history.chunk_index',
		substr(sha256('\x01'), 1, 20), '\x01');
});
$old->stop;
$refused->('one with a chunk is refused');

$old->start;
$old->safe_psql('postgres', q{
	INSERT INTO pg_history.pointers VALUES ('main', substr(sha256('\x01'), 1, 20));
});
$old->stop;
$refused->('and one with a pointer');

done_testing();
