# Copyright (c) 2026, PostgreSQL Global Development Group

# A dropped block-mapped relation keeps its relation file number until
# after the next checkpoint, as md.c keeps a dropped relation's: crash
# recovery may still replay records for it until then, which must not
# reach a new relation given the number.  Its map goes with the drop, and
# an empty main fork file keeps the number (GetNewRelFileNumber() finds
# it) until the checkpoint removes it.  A crash before the checkpoint
# replays the drop, which removes that file, also when redo made the map
# again; a standby removes the relation as it replays the drop.

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
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('deferred_unlink_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Which of the relation's files are there: its map, its main fork's.
sub files_of
{
	my ($n, $path) = @_;
	my $dir = $n->data_dir;
	return (-f "$dir/${path}_bmap" ? 'map' : 'no map') . ','
	  . (-f "$dir/$path" ? 'main' : 'no main');
}

# A dropped table's number is kept until the checkpoint, then let go of.
sql(q{CREATE TABLE gone AS SELECT generate_series(1, 10000) AS id});
my $path = sql(q{SELECT pg_relation_filepath('gone')});
is(files_of($node, $path), 'map,no main', 'a block-mapped table has a map alone');
sql('DROP TABLE gone');
is(files_of($node, $path), 'no map,main',
	'once dropped, an empty main fork file keeps its number');
is(-s $node->data_dir . "/$path", 0, 'and that file is empty');
$node->wait_for_catchup($standby, 'replay', $node->lsn('flush'));
is(files_of($standby, $path), 'no map,no main',
	'a standby removes the relation as it replays the drop');
sql('CHECKPOINT');
is(files_of($node, $path), 'no map,no main', 'the next checkpoint removes it');

# A crash before the checkpoint: redo makes the table's map again, replays
# its records, then the drop, which removes all of it.
sql(q{CREATE TABLE kept (id int); CHECKPOINT;
	  CREATE TABLE gone2 AS SELECT generate_series(1, 10000) AS id});
$path = sql(q{SELECT pg_relation_filepath('gone2')});
sql(q{INSERT INTO gone2 SELECT generate_series(1, 1000);
	  DROP TABLE gone2;
	  INSERT INTO kept SELECT generate_series(1, 5000)});
is(files_of($node, $path), 'no map,main', 'a dropped table\'s number is kept');
$node->stop('immediate');
$node->start;
is(files_of($node, $path), 'no map,no main',
	'crash recovery, making its map again, removes all of it');

# The same, the table made before the checkpoint redo starts from: redo
# finds no map, and replays the table's records onto the empty file.
sql(q{CREATE TABLE gone3 AS SELECT generate_series(1, 10000) AS id; CHECKPOINT});
$path = sql(q{SELECT pg_relation_filepath('gone3')});
sql(q{INSERT INTO gone3 SELECT generate_series(1, 1000);
	  DROP TABLE gone3;
	  INSERT INTO kept SELECT generate_series(1, 5000)});
$node->stop('immediate');
$node->start;
is(files_of($node, $path), 'no map,no main',
	'crash recovery, replaying records of a map gone, removes all of it');
is(sql('SELECT count(*) FROM kept'), '10000',
	'tables changed after the drops are intact');

$standby->stop;
$node->stop;
done_testing();
