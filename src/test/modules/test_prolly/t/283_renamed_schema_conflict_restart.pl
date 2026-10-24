# Copyright (c) 2026, PostgreSQL Global Development Group

# A schema conflict a merge kept for a table one side renamed survives a
# crash: after recovery its names on each side are still the merge
# state's, so dolt_schema_conflicts shows each side's statement under its
# own name, the commit is still refused, and --abort ends the merge.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('renamed_conflict');
$node->init;
$node->append_conf('postgresql.conf',
	"autovacuum = off\ndolt.allow_commit_conflicts = on");
$node->start;

$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA sc VERSIONED;
	CREATE TABLE sc.d (pk int PRIMARY KEY, v int);
	INSERT INTO sc.d VALUES (1, 1);
	SELECT length(dolt_commit('-Am', 'base')::text);
	SELECT dolt_branch('side');
	ALTER TABLE sc.d ALTER COLUMN v TYPE bigint;
	SELECT length(dolt_commit('-am', 'ours')::text);
	SELECT dolt_checkout('side');
	ALTER TABLE sc.d RENAME TO e;
	ALTER TABLE sc.e ALTER COLUMN v TYPE text;
	SELECT length(dolt_commit('-am', 'theirs')::text);
	SELECT dolt_checkout('main');
	SELECT dolt_merge('side');
});
my $conflicts = q{SELECT table_name || '|' || split_part(our_schema, ' (', 1) || '|'
	|| split_part(their_schema, ' (', 1) FROM dolt_schema_conflicts};
is($node->safe_psql('postgres', $conflicts),
	'sc.d|CREATE TABLE sc.d|CREATE TABLE sc.e', 'kept under ours name, each side by its own');

$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres', $conflicts),
	'sc.d|CREATE TABLE sc.d|CREATE TABLE sc.e', 'after a crash, the same');
is($node->safe_psql('postgres',
		q{SELECT string_agg(table_name || ':' || status, ',') FROM dolt_status}),
	'sc.d:schema conflict', 'the table is in conflict, under ours name alone');
my ($ret, $out, $err) = $node->psql('postgres', q{SELECT dolt_commit('-am', 'merged')});
like($err, qr/cannot commit with unresolved merge conflicts/, 'the commit is refused');
$node->safe_psql('postgres', q{SELECT dolt_merge('--abort')});
is($node->safe_psql('postgres', q{SELECT count(*) FROM dolt_schema_conflicts}), '0',
	'the abort ends it');

$node->stop;
done_testing();
