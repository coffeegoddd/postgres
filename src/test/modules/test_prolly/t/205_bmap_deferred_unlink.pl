# Copyright (c) 2026, PostgreSQL Global Development Group

# A dropped block-mapped relation keeps its map until after the next
# checkpoint, as md.c keeps a dropped relation's first segment: the map is
# what keeps its relation file number from being given out again
# (GetNewRelFileNumber()) while crash recovery may still replay records for
# the old relation.  A crash before the checkpoint replays the drop, which
# removes the map; a standby removes it as it replays the drop.  A page
# sweep reads the maps it finds, a dropped one's too, and none of their
# pages stays in shared buffers once the map is removed.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('deferred_unlink');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
shared_buffers = 16MB
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_buffercache');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('deferred_unlink_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub map_of
{
	my ($n, $path) = @_;
	return -f $n->data_dir . "/${path}_bmap" ? 1 : 0;
}

# A dropped table's map stays until the checkpoint, then goes.
sql(q{CREATE TABLE gone AS SELECT generate_series(1, 10000) AS id});
my $path = sql(q{SELECT pg_relation_filepath('gone')});
my $rel = sql(q{SELECT pg_relation_filenode('gone')});
is(map_of($node, $path), 1, 'the table has a map');
sql('DROP TABLE gone');
is(map_of($node, $path), 1, 'its map stays after the drop commits');
$node->wait_for_catchup($standby, 'replay', $node->lsn('flush'));
is(map_of($standby, $path), 0, 'a standby removes it as it replays the drop');
sql('CHECKPOINT');
is(map_of($node, $path), 0, 'the next checkpoint removes it');

# A crash before the checkpoint: redo replays the drop, and removes the map.
sql(q{CREATE TABLE gone2 AS SELECT generate_series(1, 10000) AS id;
	  CREATE TABLE kept (id int)});
$path = sql(q{SELECT pg_relation_filepath('gone2')});
sql(q{CHECKPOINT; DROP TABLE gone2;
	  INSERT INTO kept SELECT generate_series(1, 5000)});
is(map_of($node, $path), 1, 'a dropped map waits for the checkpoint');
$node->stop('immediate');
$node->start;
is(map_of($node, $path), 0, 'crash recovery removes it');
is(sql('SELECT count(*) FROM kept'), '5000',
	'a table changed after the drop is intact');

# A sweep before the checkpoint reads the dropped map; once the map is
# removed, none of its pages is left in shared buffers.
sql(q{CREATE TABLE gone3 AS SELECT generate_series(1, 10000) AS id});
$path = sql(q{SELECT pg_relation_filepath('gone3')});
$rel = sql(q{SELECT pg_relation_filenode('gone3')});
sql('DROP TABLE gone3');
sql('SELECT * FROM prolly_page_sweep()');
my $buffers = qq{SELECT count(*) FROM pg_buffercache
	WHERE relfilenode = $rel AND reldatabase =
		(SELECT oid FROM pg_database WHERE datname = 'postgres')};
cmp_ok(sql($buffers), '>', 0, 'a sweep reads the dropped table\'s map');
sql('CHECKPOINT');
is(map_of($node, $path), 0, 'the checkpoint removes the map');
is(sql($buffers), '0', 'and none of its pages stays in shared buffers');

$standby->stop;
$node->stop;
done_testing();
