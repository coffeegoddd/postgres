# Copyright (c) 2026, PostgreSQL Global Development Group

# The servers the TAP tests start put new tables on the access method under
# test.  Without this, a mistake in the settings would let every other test
# pass on stock heap tables.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $node = ProllyTest::new_node('target');
$node->start;

$node->safe_psql('postgres', 'CREATE TABLE target_probe (a int)');
is( $node->safe_psql(
		'postgres',
		"SELECT am.amname FROM pg_class c JOIN pg_am am ON am.oid = c.relam
		 WHERE c.oid = 'target_probe'::regclass"),
	ProllyTest::table_am(),
	'new tables are on the access method under test');

$node->stop;
done_testing();
