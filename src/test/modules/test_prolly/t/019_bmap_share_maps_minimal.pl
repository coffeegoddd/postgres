# Copyright (c) 2026, PostgreSQL Global Development Group

# With WAL skipped for new relations (wal_level=minimal), a share's copies
# of its source's free space map and visibility map are still durable once
# the share commits.  Nothing syncs them at commit, since shared storage has
# no pending sync, so their pages are logged as images: a crash that loses
# what was written to them and never synced (here, the files emptied while
# the server is down) leaves redo to write them again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('share_maps_minimal');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
wal_level = minimal
max_wal_senders = 0
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_walinspect;
	CREATE TABLE t (a int8, b int8);
	INSERT INTO t SELECT g, g FROM generate_series(1, 20000) g;
	VACUUM (FREEZE) t;
	CHECKPOINT;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
my $dir = $node->data_dir . '/base/'
  . sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
ok(-s "$dir/${t}_vm" && -s "$dir/${t}_fsm",
	'the source has a visibility map and a free space map');

my $before = sql(q{SELECT pg_current_wal_insert_lsn()});
my $copy = sql(qq{SELECT test_bmap_share($t)});
# (WAL flushed by a transaction that logs a change: the helper's commits none)
sql(q{CREATE TABLE flushed ()});
is( sql(
		qq{SELECT count(DISTINCT relforknumber)
	FROM pg_get_wal_block_info('$before', pg_current_wal_insert_lsn())
	WHERE relfilenode = $copy AND relforknumber IN (1, 2) AND block_fpi_length > 0}
	),
	'2',
	'the copy\'s maps are logged as images');

# A map page's contents, past its header (whose LSN and checksum are the
# copy's own).
sub map_bits
{
	my ($path) = @_;
	my $data = slurp_file($path);
	my $bits = '';
	for (my $off = 0; $off < length($data); $off += 8192)
	{
		$bits .= substr($data, $off + 24, 8192 - 24);
	}
	return $bits;
}

$node->stop('immediate');
# The copied pages were written but never synced, so an operating system
# crash may lose them: the files are emptied, as if it had.
foreach my $fork ('vm', 'fsm')
{
	open(my $fh, '>', "$dir/${copy}_$fork") or die "could not truncate: $!";
	close($fh);
}
$node->start;
# (the end-of-recovery checkpoint wrote the pages redo restored)
foreach my $fork ('vm', 'fsm')
{
	ok(-s "$dir/${copy}_$fork", "the copy's $fork fork has pages after recovery");
	ok(map_bits("$dir/${copy}_$fork") eq map_bits("$dir/${t}_$fork"),
		"and they are the source's");
}
is(sql(qq{SELECT test_bmap_rows_digest($copy) = test_bmap_rows_digest($t)}),
	't', 'the copy keeps its rows');

$node->stop;
done_testing();
