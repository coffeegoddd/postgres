# Copyright (c) 2026, PostgreSQL Global Development Group

# A database copied from a template has the template's rows, with either
# copy strategy, and keeps them across a crash and on a standby.  A
# database created with a dropped one's OID holds nothing of the dropped
# one.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $am = $ENV{PROLLY_AM} // 'prolly_d1';
my $conf = "default_table_access_method = '$am'\n"
  . "wal_consistency_checking = 'all'\n";

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $conf);
$primary->start;
$primary->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', $conf);
$standby->start;

$primary->safe_psql('postgres', 'CREATE DATABASE src');
$primary->safe_psql('src', q{
	CREATE TABLE t (id int PRIMARY KEY, v text);
	INSERT INTO t VALUES (1, 'one'), (2, 'two'), (3, 'three');
	CREATE TABLE k (a int);
	INSERT INTO k VALUES (1), (1);
});

my $rows = q{SELECT (SELECT string_agg(v, ',' ORDER BY id) FROM t),
	(SELECT count(*) FROM k)};
my $expect = 'one,two,three|2';

for my $strategy ('file_copy', 'wal_log')
{
	$primary->safe_psql('postgres',
		"CREATE DATABASE copy_$strategy TEMPLATE src STRATEGY $strategy");
	is($primary->safe_psql("copy_$strategy", $rows), $expect,
		"a $strategy copy has the template's rows");
}
$primary->safe_psql('copy_wal_log', q{INSERT INTO t VALUES (4, 'four')});
is($primary->safe_psql('src', $rows), $expect,
	'writes to a copy leave the template unchanged');

my $oid = $primary->safe_psql('postgres',
	q{SELECT oid FROM pg_database WHERE datname = 'copy_wal_log'});
$primary->safe_psql('postgres', 'DROP DATABASE copy_wal_log');
$primary->safe_psql('postgres',
	"CREATE DATABASE again OID = $oid TEMPLATE src");
is($primary->safe_psql('again', $rows), $expect,
	'a database with a dropped one\'s OID has only its template\'s rows');

$primary->wait_for_replay_catchup($standby);
for my $db ('copy_file_copy', 'again')
{
	is($standby->safe_psql($db, $rows), $expect,
		"the standby has the rows of $db");
}

$primary->stop('immediate');
$primary->start;
for my $db ('copy_file_copy', 'again')
{
	is($primary->safe_psql($db, $rows), $expect,
		"the rows of $db survive a crash");
}

done_testing();
