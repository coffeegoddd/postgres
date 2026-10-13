# Copyright (c) 2026, PostgreSQL Global Development Group

# Canonical form across clusters: two separate clusters build the same
# schema and data by different histories, with OIDs that differ (one
# cluster creates and drops objects first), and their commits have the same
# root hash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $am = $ENV{PROLLY_AM} // 'prolly_d1';

my @nodes;
for my $name ('one', 'two')
{
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->append_conf('postgresql.conf',
		"default_table_access_method = '$am'");
	$node->start;
	push @nodes, $node;
}
my ($one, $two) = @nodes;

$one->safe_psql('postgres', q(
	CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
	CREATE TABLE people (id int PRIMARY KEY, name text, mood mood, tags text[]);
	CREATE INDEX people_name ON people (name);
	INSERT INTO people
		SELECT g, 'p' || g, (ARRAY['sad', 'ok', 'happy']::mood[])[g % 3 + 1],
			   ARRAY['t' || (g % 4)]
		FROM generate_series(1, 300) g;
	SELECT prolly_commit('one');
));

$two->safe_psql('postgres', q(
	CREATE TABLE junk AS SELECT g FROM generate_series(1, 10) g;
	CREATE TYPE junk_type AS ENUM ('x');
	DROP TABLE junk;
	DROP TYPE junk_type;
	CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
	CREATE TABLE people (id int PRIMARY KEY, name text, mood mood, tags text[], extra int);
	INSERT INTO people
		SELECT g, 'p' || g, (ARRAY['sad', 'ok', 'happy']::mood[])[g % 3 + 1],
			   ARRAY['t' || (g % 4)], g
		FROM generate_series(300, 1, -1) g;
	ALTER TABLE people DROP COLUMN extra;
	CREATE INDEX people_name ON people (name);
	SELECT prolly_commit('two');
));

isnt($one->safe_psql('postgres', "SELECT 'mood'::regtype::oid"),
	$two->safe_psql('postgres', "SELECT 'mood'::regtype::oid"),
	'the clusters have different OIDs');
is($one->safe_psql('postgres', "SELECT prolly_root_hash('main')"),
	$two->safe_psql('postgres', "SELECT prolly_root_hash('main')"),
	'same content gives the same root hash across clusters');

$_->stop for @nodes;
done_testing();
