# Copyright (c) 2026, PostgreSQL Global Development Group

# Block map statistics are kept by relation file: its database, its
# tablespace and its file number.  ALTER DATABASE SET TABLESPACE moves the
# database's files in its default tablespace to another, where they count
# under the new tablespace: the old one's entries are dropped with the
# files, not left until the database is.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('stats_movedb');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
allow_in_place_tablespaces = on
autovacuum = off
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE DATABASE d;
	CREATE TABLESPACE ts LOCATION '';
});

sub dsql { return $node->safe_psql('d', $_[0]); }

# A table shared, then changed: its pages copied, counted by its file.
dsql(q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, repeat('t', 200) FROM generate_series(1, 2000) g;
});
dsql(q{SELECT test_bmap_share(pg_relation_filenode('t'))});
dsql(q{UPDATE t SET b = 'u'; SELECT pg_stat_force_next_flush()});
my $db = $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'd'});
my $key = dsql(q{
	SELECT (SELECT dattablespace FROM pg_database WHERE datname = 'd')::int8 << 32
	       | relfilenode::int8 FROM pg_class WHERE relname = 't'});
is(dsql(qq{SELECT pg_stat_have_stats('block_map', $db, $key)}), 't',
	'the table\'s file has block map statistics');

# The database moved to another tablespace.
$node->safe_psql('postgres', q{ALTER DATABASE d SET TABLESPACE ts});
is($node->safe_psql('postgres', qq{SELECT pg_stat_have_stats('block_map', $db, $key)}),
	'f', 'the entry under the old tablespace is dropped with its files');
is(dsql(q{SELECT count(*) FROM t WHERE b = 'u'}), '2000',
	'and the table reads its rows from the new one');

$node->stop;
done_testing();
