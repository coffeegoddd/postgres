# Copyright (c) 2026, PostgreSQL Global Development Group

# Canonical form across clusters: two separate clusters build the same
# schema and data by different histories, with OIDs that differ (one
# cluster creates and drops objects first), and their commits have the same
# root hash.  On the tree engines objects are committed where they are; on
# prolly they are in a versioned schema, committed with dolt_commit, and
# the second cluster's history also goes through branches and a merge.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $am = $ENV{PROLLY_AM} // 'prolly_d1';
my $copies = $am eq 'prolly';
my $s = $copies ? 'app.' : '';

my @nodes;
for my $name ('one', 'two')
{
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->append_conf('postgresql.conf',
		"default_table_access_method = '$am'");
	$node->start;
	$node->safe_psql('postgres', 'CREATE SCHEMA app VERSIONED') if $copies;
	push @nodes, $node;
}
my ($one, $two) = @nodes;
my $commit = $copies ? "SELECT dolt_commit('-Am', 'commit')" : "SELECT prolly_commit('commit')";
my $root = $copies ? "SELECT prolly_root_hash('HEAD')" : "SELECT prolly_root_hash('main')";

$one->safe_psql('postgres', qq(
	CREATE TYPE ${s}mood AS ENUM ('sad', 'ok', 'happy');
	CREATE TABLE ${s}people (id int PRIMARY KEY, name text, mood ${s}mood, tags text[]);
	CREATE INDEX people_name ON ${s}people (name);
	INSERT INTO ${s}people
		SELECT g, 'p' || g, (ARRAY['sad', 'ok', 'happy']::${s}mood[])[g % 3 + 1],
			   ARRAY['t' || (g % 4)]
		FROM generate_series(1, 300) g;
	$commit;
));

$two->safe_psql('postgres', qq(
	CREATE TABLE ${s}junk AS SELECT g FROM generate_series(1, 10) g;
	CREATE TYPE ${s}junk_type AS ENUM ('x');
	DROP TABLE ${s}junk;
	DROP TYPE ${s}junk_type;
	CREATE TYPE ${s}mood AS ENUM ('sad', 'ok', 'happy');
	CREATE TABLE ${s}people (id int PRIMARY KEY, name text, mood ${s}mood, tags text[], extra int);
	INSERT INTO ${s}people
		SELECT g, 'p' || g, (ARRAY['sad', 'ok', 'happy']::${s}mood[])[g % 3 + 1],
			   ARRAY['t' || (g % 4)], g
		FROM generate_series(300, 151, -1) g;
	ALTER TABLE ${s}people DROP COLUMN extra;
	CREATE INDEX people_name ON ${s}people (name);
	$commit;
));
if ($copies)
{
	# the rest of the rows on a branch, merged in
	$two->safe_psql('postgres', q(
		SELECT dolt_branch('rest');
		SELECT dolt_checkout('rest');
		INSERT INTO app.people
			SELECT g, 'p' || g, (ARRAY['sad', 'ok', 'happy']::app.mood[])[g % 3 + 1],
				   ARRAY['t' || (g % 4)]
			FROM generate_series(1, 150) g;
		SELECT dolt_commit('-am', 'the rest');
		SELECT dolt_checkout('main');
		SELECT dolt_merge('rest');
	));
}
else
{
	$two->safe_psql('postgres', q(
		INSERT INTO people
			SELECT g, 'p' || g, (ARRAY['sad', 'ok', 'happy']::mood[])[g % 3 + 1],
				   ARRAY['t' || (g % 4)]
			FROM generate_series(1, 150) g;
		SELECT prolly_commit('the rest');
	));
}

isnt($one->safe_psql('postgres', "SELECT '${s}mood'::regtype::oid"),
	$two->safe_psql('postgres', "SELECT '${s}mood'::regtype::oid"),
	'the clusters have different OIDs');
is($one->safe_psql('postgres', $root), $two->safe_psql('postgres', $root),
	'same content gives the same root hash across clusters');

$_->stop for @nodes;
done_testing();
