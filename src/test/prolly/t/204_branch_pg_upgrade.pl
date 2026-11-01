# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_upgrade refuses a cluster with branches: its dump and lists of
# relations, made on no branch, would leave out every branch's objects.
# A cluster without any upgrades.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $old = ProllyTest::new_node('upgrade_old');
my $new = ProllyTest::new_node('upgrade_new');
$old->start;
$old->safe_psql('postgres', 'CREATE TABLE plain (x int)');
$old->stop;

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
my $dir = PostgreSQL::Test::Utils::tempdir;
chdir $dir;

command_ok(\@check, 'a cluster without branches may be upgraded');

$old->start;
my $b = $old->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$old->stop;
my $refused = sub {
	my ($name) = @_;
	my ($stdout, $stderr) = run_command(\@check);
	like($stdout . $stderr, qr/Database "postgres" has branches, which cannot be upgraded yet/, $name);
};
$refused->('a cluster with a branch allocated is refused');

$old->start;
$old->safe_psql('postgres', "SET catalog_branch = $b; CREATE SCHEMA app VERSIONED; CREATE TABLE app.t (x int)");
$old->stop;
$refused->('and one with a versioned schema');

done_testing();
