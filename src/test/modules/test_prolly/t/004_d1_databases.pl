# Copyright (c) 2026, PostgreSQL Global Development Group

# Engine D1 tables in databases copied from a template, with either
# strategy, and in a database dropped and created again with the same OID;
# on the primary, after a crash, and on a standby.
#
# A database's tables are found from its branch heads, which CREATE
# DATABASE copies from the template's and DROP DATABASE forgets.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $conf = "default_table_access_method = prolly_d1\n";

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
	CREATE INDEX k_a ON k (a);
});

my $rows = q{SELECT (SELECT string_agg(v, ',' ORDER BY id) FROM t),
	(SELECT count(*) FROM k)};
my $expect = 'one,two,three|2';

for my $strategy ('file_copy', 'wal_log')
{
	$primary->safe_psql('postgres',
		"CREATE DATABASE clone_$strategy TEMPLATE src STRATEGY $strategy");
	is($primary->safe_psql("clone_$strategy", $rows), $expect,
		"a $strategy copy has the template's rows");
}

# The copies are separate: writes to one don't show in the others.
$primary->safe_psql('clone_wal_log', q{INSERT INTO t VALUES (4, 'four')});
is($primary->safe_psql('clone_wal_log', $rows), 'one,two,three,four|2',
	'a copy takes writes');
is($primary->safe_psql('src', $rows), $expect, 'the template is unchanged');
is($primary->safe_psql('clone_file_copy', $rows), $expect,
	'the other copy is unchanged');

# Dropped, then created again with the same OID: the new database starts
# from its template, not from what the dropped one held.
my $oid = $primary->safe_psql('postgres',
	q{SELECT oid FROM pg_database WHERE datname = 'clone_file_copy'});
$primary->safe_psql('postgres', 'DROP DATABASE clone_file_copy');
$primary->safe_psql('postgres',
	"CREATE DATABASE clone_again OID = $oid TEMPLATE src STRATEGY file_copy");
is($primary->safe_psql('clone_again', $rows), $expect,
	'a database created with a dropped one\'s OID has its template\'s rows');

# The same from a template without prolly tables.
$primary->safe_psql('clone_again', q{INSERT INTO t VALUES (5, 'five')});
my $empty_oid = $primary->safe_psql('postgres',
	q{SELECT oid FROM pg_database WHERE datname = 'clone_wal_log'});
$primary->safe_psql('postgres', 'DROP DATABASE clone_wal_log');
$primary->safe_psql('postgres',
	"CREATE DATABASE clone_empty OID = $empty_oid TEMPLATE template0");
is($primary->safe_psql('clone_empty',
	q{SELECT count(*) FROM pg_class WHERE relname = 't'}), '0',
	'a database from template0 has no tables');
$primary->safe_psql('clone_empty', q{
	CREATE TABLE t (id int PRIMARY KEY, v text);
	INSERT INTO t VALUES (10, 'ten');
});
is($primary->safe_psql('clone_empty', 'SELECT string_agg(v, \',\') FROM t'),
	'ten', 'and holds only its own rows');

my %final = (
	src => $expect,
	clone_again => 'one,two,three,five|2',
);

$primary->wait_for_replay_catchup($standby);
for my $db (sort keys %final)
{
	is($standby->safe_psql($db, $rows), $final{$db},
		"the standby replays database $db");
}

$primary->stop('immediate');
$primary->start;
for my $db (sort keys %final)
{
	is($primary->safe_psql($db, $rows), $final{$db},
		"database $db survives a crash");
}

$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->restart;
for my $db (sort keys %final)
{
	is($primary->safe_psql($db, $rows), $final{$db},
		"database $db survives a checkpoint and restart");
}

done_testing();
