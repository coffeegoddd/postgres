# Copyright (c) 2026, PostgreSQL Global Development Group

# Canonical form across clusters: two separate clusters build the same
# schema and data by different histories (the rows inserted in opposite
# orders, one cluster creating and dropping objects first, so the OIDs
# differ), and their commits have the same root hash; a row changed
# changes it.  The objects are in a versioned schema, committed with
# dolt_commit, and the second cluster's history also goes through branches
# and a three-way merge.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $am = $ENV{PROLLY_AM} // 'prolly';

my @nodes;
for my $name ('one', 'two')
{
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->start;
	ProllyTest::setup_target($node, 'postgres', $am);
	push @nodes, $node;
}
my ($one, $two) = @nodes;
my $commit = "SELECT dolt_commit('-Am', 'commit')";
my $root = "SELECT prolly_root_hash('main')";
sub rows
{
	my ($from, $to, $extra) = @_;
	my $step = $from > $to ? -1 : 1;
	return "INSERT INTO people SELECT g, 'p' || g, (ARRAY['sad', 'ok', 'happy']::mood[])[g % 3 + 1], "
	  . "ARRAY['t' || (g % 4)]" . ($extra ? ', g' : '')
	  . " FROM generate_series($from, $to, $step) g";
}

$one->safe_psql('postgres', qq(
	CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
	CREATE TABLE people (id int PRIMARY KEY, name text, mood mood, tags text[]);
	CREATE INDEX people_name ON people (name);
	@{[rows(1, 300)]};
	$commit;
));

# the same, the other way round: rows from the top down, in three parts,
# the last two on two branches merged
$two->safe_psql('postgres', qq(
	CREATE TABLE junk AS SELECT g FROM generate_series(1, 10) g;
	CREATE TYPE junk_type AS ENUM ('x');
	DROP TABLE junk;
	DROP TYPE junk_type;
	CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
	CREATE TABLE people (id int PRIMARY KEY, name text, mood mood, tags text[], extra int);
	@{[rows(300, 201, 1)]};
	ALTER TABLE people DROP COLUMN extra;
	CREATE INDEX people_name ON people (name);
	$commit;
));
$two->safe_psql('postgres', qq(
	SELECT dolt_branch('rest');
	SELECT dolt_checkout('rest');
	@{[rows(150, 1)]};
	SELECT dolt_commit('-am', 'the low rows');
	SELECT dolt_checkout('main');
	@{[rows(200, 151)]};
	SELECT dolt_commit('-am', 'the middle rows');
));
is($two->safe_psql('postgres', q{SELECT (dolt_merge('rest', '-m', 'merged'))[2]}),
	'0', 'the merge is three-way, not a fast-forward');

isnt($one->safe_psql('postgres', "SELECT 'mood'::regtype::oid"),
	$two->safe_psql('postgres', "SELECT 'mood'::regtype::oid"),
	'the clusters have different OIDs');
is($one->safe_psql('postgres', $root), $two->safe_psql('postgres', $root),
	'same content gives the same root hash across clusters');

# and the root is of the content: a row changed gives another
$two->safe_psql('postgres', qq(
	UPDATE people SET name = 'changed' WHERE id = 1;
	$commit;
));
isnt($one->safe_psql('postgres', $root), $two->safe_psql('postgres', $root),
	'a row changed gives another root hash');

$_->stop for @nodes;
done_testing();
