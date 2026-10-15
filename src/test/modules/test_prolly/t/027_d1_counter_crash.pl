# Copyright (c) 2026, PostgreSQL Global Development Group

# A table's TID counter page is made on its first insert, by extending the
# main fork (the new, empty block reaches the file at once) and filling it
# in under WAL.  A crash that loses that WAL leaves an empty block 0: it is
# made again, rather than refused as an invalid counter (Postgres's
# 022_crash_temp_files recovery test).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('counter_crash');
$node->init;
$node->append_conf('postgresql.conf', qq{
default_table_access_method = prolly_d1
wal_writer_delay = 10s
});
$node->start;
$node->safe_psql('postgres', 'CREATE TABLE tc (id int PRIMARY KEY)');

# The first insert, uncommitted, then a crash before its WAL is written.
my $s = $node->background_psql('postgres', on_error_stop => 0);
$s->query_safe('BEGIN; INSERT INTO tc VALUES (1);');
$node->stop('immediate');
$s->quit;
$node->start;

$node->safe_psql('postgres', 'INSERT INTO tc VALUES (2)');
is($node->safe_psql('postgres', 'SELECT string_agg(id::text, \',\') FROM tc'), '2',
	'a table whose first insert was lost in a crash takes inserts');
my $tid = $node->safe_psql('postgres', 'SELECT ctid FROM tc');
$node->safe_psql('postgres', 'INSERT INTO tc VALUES (3)');
isnt($node->safe_psql('postgres', 'SELECT ctid FROM tc WHERE id = 3'), $tid,
	'and gives out distinct TIDs');

$node->stop;
done_testing();
