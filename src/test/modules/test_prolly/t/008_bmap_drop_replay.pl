# Copyright (c) 2026, PostgreSQL Global Development Group

# A block-mapped relation dropped on a standby leaves none of its buffers:
# its map's pages too, which smgrexists() hides.  (A buffer left would be
# written out later, to a file gone, and stop the standby's recovery.)

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('drops');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION pg_buffercache');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('drops_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA d;
	CREATE TABLE d.t (a int8, b int8);
	INSERT INTO d.t SELECT g, g FROM generate_series(1, 20) g});
my $rel = sql(q{SELECT pg_relation_filenode('d.t')});
sql(q{UPDATE d.t SET b = -b WHERE a % 5 = 0});
# (a small relation, with no index, no TOAST table and no free space map,
# and a change of a page it has last: recovery then knows the length of
# everything the drop drops, and drops their buffers by it)
sql(q{CHECKPOINT; UPDATE d.t SET b = 0 WHERE a = 1});
$node->wait_for_catchup($standby, 'replay', $node->lsn('flush'));

# (the map's pages are the relation's fork 4)
my $buffers = qq{SELECT coalesce(string_agg(DISTINCT relforknumber::text, ',' ORDER BY relforknumber::text), '')
	FROM pg_buffercache WHERE relfilenode = $rel};
is($standby->safe_psql('postgres', $buffers), '0,4',
	'a standby has the relation\'s pages, and its map\'s, in buffers');

sql('DROP TABLE d.t');
$node->wait_for_catchup($standby, 'replay', $node->lsn('flush'));
is($standby->safe_psql('postgres', $buffers), '',
	'once its drop is replayed, it has none of them');

$standby->stop;
$node->stop;
done_testing();
