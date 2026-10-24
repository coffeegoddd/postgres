# Copyright (c) 2026, PostgreSQL Global Development Group

# A status takes the definitions its backend last read while nothing they're
# of changed: another backend's catalog change, landing between two of one
# backend's statuses, makes the second read them again and show it.  What
# the definitions leave out (an object the canonical form doesn't describe)
# is taken with them.  debug_prolly_verify_fold_caches reads them again
# and compares, tree and objects left out.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('status_definitions');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
debug_prolly_verify_fold_caches = on
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.a (id int PRIMARY KEY, v int);
	CREATE TABLE s.b (id int PRIMARY KEY, v int);
	INSERT INTO s.a VALUES (1, 0);
	INSERT INTO s.b VALUES (1, 0);
	SELECT dolt_commit('-Am', 'base');
});

my $reader = $node->background_psql('postgres');
sub status
{
	return $reader->query_safe(
		q{SELECT string_agg(table_name || ':' || status, ',' ORDER BY table_name)
		  FROM dolt_status});
}
sub reused
{
	$reader->query_safe(q{SELECT pg_stat_force_next_flush()});
	return $reader->query_safe(
		q{SELECT value FROM pg_stat_prolly WHERE name = 'definitions_reused'});
}

# A row written: the reader's statuses show it.
sql(q{UPDATE s.a SET v = 1});
is(status(), 's.a:modified', 'the first status');
is(status(), 's.a:modified', 'the second the same');

# Another backend's DDL between two of the reader's statuses: read again.
sql(q{ALTER TABLE s.b ADD COLUMN w int});
my $before = reused();
is(status(), 's.a:modified,s.b:modified',
	'another backend\'s DDL is seen by the next status');
is(reused() - $before, 0, 'which took no definitions read before it');
is(status(), 's.a:modified,s.b:modified', 'and by the one after');

# A new table from another backend, the same.
sql(q{CREATE TABLE s.c (id int PRIMARY KEY)});
is(status(), 's.a:modified,s.b:modified,s.c:new table',
	'another backend\'s new table is seen');

# What the definitions leave out is taken with them (verified).
sql(q{CREATE CONVERSION s.conv FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8});
is(status(), 'conversion s.conv:not covered,s.a:modified,s.b:modified,s.c:new table',
	'an object left out is listed');
$before = reused();
is(status(), 'conversion s.conv:not covered,s.a:modified,s.b:modified,s.c:new table',
	'and listed again from the definitions taken');
is(reused() - $before, 1, 'which were taken');

$reader->quit;
done_testing();
