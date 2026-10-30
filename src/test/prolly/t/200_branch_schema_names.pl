# Copyright (c) 2026, PostgreSQL Global Development Group

# A schema name is shared or versioned, never both, even when two sessions
# make the two kinds at once: the second waits for the first, and fails if
# it committed.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $node = ProllyTest::new_node('names');
$node->start;

my $b1 = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');

# Each case: the first session makes the schema and holds its transaction
# open; the second tries the other kind; then the first commits.
sub race
{
	my ($name, $first_branch, $first_sql, $second_branch, $second_sql) = @_;

	my $first = $node->background_psql('postgres');
	my $second = $node->background_psql('postgres', on_error_stop => 0);
	$first->query_safe("SET catalog_branch = $first_branch");
	$second->query_safe("SET catalog_branch = $second_branch");

	$first->query_safe('BEGIN');
	$first->query_safe($first_sql);

	$second->query_until(qr/started/, "\\echo started\n$second_sql;\n");
	ok( $node->poll_query_until(
			'postgres',
			"SELECT count(*) > 0 FROM pg_stat_activity
			 WHERE wait_event_type = 'Lock' AND query = '$second_sql;'"),
		"$name: the second session waits for the first");

	$first->query_safe('COMMIT');
	$second->query('SELECT 1');
	like($second->{stderr}, qr/schema "\w+" already exists/,
		"$name: and fails once the first commits");

	$first->quit;
	$second->quit;
}

race('shared, then versioned', 0, 'CREATE SCHEMA s1', $b1,
	'CREATE SCHEMA s1 VERSIONED');
race('versioned, then shared', $b1, 'CREATE SCHEMA s2 VERSIONED', 0,
	'CREATE SCHEMA s2');

is( $node->safe_psql(
		'postgres',
		"SET catalog_branch = $b1;
		 SELECT string_agg(nspname || ':' || (nspbranch <> 0), ',' ORDER BY nspname)
		 FROM pg_namespace WHERE nspname IN ('s1', 's2')"),
	's1:false,s2:true',
	'one kind of schema per name');

$node->stop;
done_testing();
