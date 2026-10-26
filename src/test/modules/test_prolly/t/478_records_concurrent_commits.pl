# Copyright (c) 2026, PostgreSQL Global Development Group

# Commits' records of relations are kept in one tree apart from the refs,
# whose root one refs entry names: every commit changes it.  Two
# transactions committing on two branches at once each add their records
# to the tree as they saw it; the one publishing second, rebuilt on the
# root the first moved, adds its records to the tree the first left, so
# both commits keep their records.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('records_concurrent_commits');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE rc');
$node->safe_psql('rc', 'CREATE EXTENSION test_prolly');

sub sql { return $node->safe_psql('rc', $_[0]); }

sql(q{CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t VALUES (1, 'base')});
sql(q{SELECT dolt_commit('-Am', 'base')});
sql(q{SELECT dolt_branch('a'); SELECT dolt_branch('b')});

my @s = map { $node->background_psql('rc') } (0, 1);
$s[0]->query_safe(q{SELECT dolt_checkout('a')});
$s[1]->query_safe(q{SELECT dolt_checkout('b')});
# (each commits in an open transaction: its refs changes published at its
# end)
for my $i (0, 1)
{
	$s[$i]->query_safe(q{BEGIN});
	$s[$i]->query_safe(qq{UPDATE s.t SET v = 'on $i'});
	$s[$i]->query_safe(qq{SELECT dolt_commit('-am', 'c$i')});
}
$s[0]->query_safe(q{COMMIT});
$s[1]->query_safe(q{COMMIT});
$_->quit for @s;

my $a = sql(q{SELECT dolt_hashof('a')});
my $b = sql(q{SELECT dolt_hashof('b')});
is(sql(qq{SELECT test_refs_record('$a') IS NOT NULL}), 't',
	"the first to publish keeps its commit's record");
is(sql(qq{SELECT test_refs_record('$b') IS NOT NULL}), 't',
	"the second, rebuilt on the first's, keeps its own");
is(sql(qq{SELECT test_refs_record(dolt_hashof('main')) IS NOT NULL}), 't',
	"and the base's record stays");

# A history read on each branch goes by its record: the table's versions
# are its relation's.
is(sql(q{SELECT dolt_checkout('b') IS NULL;
	SELECT count(*) FROM dolt_history_table(NULL::s.t)}), "f\n2",
	"b's history reads through its commit's record");

# The same in a new database, before any commit is recorded: the second
# to publish rebuilds from a base with no records' tree at all.
$node->safe_psql('postgres', 'CREATE DATABASE rc2');
$node->safe_psql('rc2', 'CREATE EXTENSION test_prolly');
$node->safe_psql('rc2', q{SELECT dolt_branch('b')});
my @n = map { $node->background_psql('rc2') } (0, 1);
$n[1]->query_safe(q{SELECT dolt_checkout('b')});
$n[0]->query_safe(q{BEGIN});
$n[0]->query_safe(q{SELECT dolt_commit('--allow-empty', '-m', 'on main')});
$n[1]->query_safe(q{SELECT dolt_commit('--allow-empty', '-m', 'on b')});
$n[0]->query_safe(q{COMMIT});
is($n[0]->{stderr}, '', 'the second commit, from a base with no records, publishes');
$_->quit for @n;
for my $br ('main', 'b')
{
	is( $node->safe_psql(
			'rc2', qq{SELECT test_refs_record(dolt_hashof('$br')) IS NOT NULL}),
		't',
		"${br}'s commit keeps its record");
}

$node->stop;
done_testing();
