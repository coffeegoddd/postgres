# Copyright (c) 2026, PostgreSQL Global Development Group

# Block-mapped pages evicted under pressure (few buffers): a change that
# copies a page as of a fold extends the changes list, and the extension
# may evict a dirty page of the same relation whose entry is on the map
# page of the page it changes.  The write locks that map page to look the
# page up, so the change locks it only after the list's pages.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('evictions');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{autovacuum = off
shared_buffers = 16});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE ev');
$node->safe_psql('ev', q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.t (a int8 PRIMARY KEY, b int8);
	INSERT INTO cow.t SELECT g, g FROM generate_series(1, 3000) g;
});
$node->safe_psql('ev', 'CHECKPOINT');

sub sql { return $node->safe_psql('ev', $_[0]); }

my $rel = sql(q{SELECT pg_relation_filenode('cow.t')});
is(sql(q{SELECT count(*) || ' ' || sum(b) FROM cow.t}), '3000 4501500', 'read');
sql(qq{SELECT test_bmap_fold_begin($rel)});
sql(q{UPDATE cow.t SET b = b + 1 WHERE a % 100 = 0});
is(sql(q{SELECT count(*) || ' ' || sum(b) FROM cow.t}), '3000 4501530',
	'a change evicting a page under its own map page goes through');

$node->stop;
done_testing();
