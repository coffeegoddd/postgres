# Copyright (c) 2026, PostgreSQL Global Development Group

# dolt_reflog after a collection that keeps the newest sealed segments for
# a logical replication slot: their moves are still recorded, and listed
# from the root the collected segments before them end at.  So a move
# among them that changed no ref (a working set's checkpoint) lists
# nothing, as after a collection that keeps none.  With every sealed
# segment kept, or the collected ones holding no move, that root is the
# one an earlier collection left; with kept segments that aren't the
# newest (one whose seal's position isn't known), it isn't known, and the
# first root kept lists every ref it names.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('reflog_gc_slot');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;

# A database of its own for each case: its chunk store, and a slot of it.
sub setup
{
	my ($db) = @_;

	$node->safe_psql('postgres', "CREATE DATABASE $db") if $db ne 'postgres';
	$node->safe_psql($db, q{
		CREATE EXTENSION test_prolly;
		CREATE SCHEMA rs VERSIONED;
		CREATE TABLE rs.t (id int PRIMARY KEY);
		SELECT dolt_commit('-Am', 'base');
	});
}

sub reflog
{
	return $_[0]->safe_psql($_[1],
		q{SELECT string_agg(commit_message, ',') FROM dolt_reflog('main')});
}

# The newest sealed segment kept: the collected one's last root is the base.
setup('postgres');
$node->safe_psql('postgres', q{SELECT test_chunk_seal()});
$node->safe_psql('postgres',
	q{SELECT pg_create_logical_replication_slot('rs', 'test_decoding')});
$node->safe_psql('postgres', q{
	INSERT INTO rs.t VALUES (1);
	SELECT prolly_checkpoint();
	SELECT dolt_commit('-am', 'row 1');
	SELECT dolt_gc();
});
is($node->safe_psql('postgres', q{SELECT count(*) FROM test_chunk_segments()}), '2',
	'the collection kept the sealed segment the slot may decode');
is(reflog($node, 'postgres'), 'row 1',
	"the kept segment's moves listed from the collected ones' root: the checkpoint lists nothing");
$node->safe_psql('postgres', q{SELECT pg_drop_replication_slot('rs')});

# Every sealed segment kept: the base an earlier collection left.
setup('every');
$node->safe_psql('every', q{SELECT dolt_gc()});
$node->safe_psql('every',
	q{SELECT pg_create_logical_replication_slot('rs_every', 'test_decoding')});
$node->safe_psql('every', q{
	INSERT INTO rs.t VALUES (1);
	SELECT prolly_checkpoint();
	SELECT dolt_commit('-am', 'row 1');
	SELECT dolt_gc();
});
is(reflog($node, 'every'), 'row 1',
	'every sealed segment kept: the moves listed from the earlier collection\'s base');
$node->safe_psql('every', q{SELECT pg_drop_replication_slot('rs_every')});

# The collected segments moved nothing: the earlier collection's base.
setup('still');
$node->safe_psql('still', q{SELECT dolt_gc()});
$node->safe_psql('still', q{SELECT test_chunk_seal()});
$node->safe_psql('still',
	q{SELECT pg_create_logical_replication_slot('rs_still', 'test_decoding')});
$node->safe_psql('still', q{
	INSERT INTO rs.t VALUES (1);
	SELECT prolly_checkpoint();
	SELECT dolt_commit('-am', 'row 1');
	SELECT dolt_gc();
});
is(reflog($node, 'still'), 'row 1',
	'the collected segments holding no move: the moves listed from the earlier base');
$node->safe_psql('still', q{SELECT pg_drop_replication_slot('rs_still')});

# A kept segment that isn't among the newest: one whose seal's position
# isn't known (a manifest of segments sealed by a build that didn't record
# it), patched in.  The base isn't known: the first root kept lists main.
setup('gap');
$node->safe_psql('gap', q{
	SELECT test_chunk_seal();
	SELECT prolly_checkpoint();
	SELECT test_chunk_seal();
	INSERT INTO rs.t VALUES (1);
	SELECT dolt_commit('-am', 'row 1');
	SELECT test_chunk_seal();
});
$node->safe_psql('gap',
	q{SELECT pg_create_logical_replication_slot('rs_gap', 'test_decoding')});
my $oid = $node->safe_psql('gap', q{SELECT oid FROM pg_database WHERE datname = 'gap'});
$node->stop;
unknown_seal($node->data_dir . "/base/$oid/prolly_manifest", 1);
$node->start;
$node->safe_psql('gap', q{SELECT dolt_gc()});
# (row 1's move was in a collected segment: forgotten.  The kept one's
# checkpoint changed no ref, and lists main only as the base isn't known.)
is(reflog($node, 'gap'), 'base',
	'a kept segment not among the newest: the base unknown, the first root kept lists main');
$node->safe_psql('gap', q{SELECT pg_drop_replication_slot('rs_gap')});

$node->stop;
done_testing();

# CRC-32C (Castagnoli), as the manifest's last four bytes are of the rest.
sub crc32c
{
	my $crc = 0xFFFFFFFF;
	for my $byte (unpack('C*', $_[0]))
	{
		$crc ^= $byte;
		$crc = ($crc >> 1) ^ (($crc & 1) ? 0x82F63B78 : 0) for 1 .. 8;
	}
	return $crc ^ 0xFFFFFFFF;
}

# Make the manifest's segment n (oldest first) one sealed at an unknown
# position: its sealed_at zero, the manifest's CRC made again.  (Magic,
# version, the store's 19-byte format, gc generation, last id, root and
# base each a length byte then the hash, the files of 21 bytes each, then
# the segments: an id and a position, 8 bytes each.)
sub unknown_seal
{
	my ($path, $n) = @_;
	open(my $fh, '+<:raw', $path) or die "open $path: $!";
	local $/;
	my $all = <$fh>;
	my $p = 8 + 4 + 19 + 4 + 8;
	$p += 1 + unpack('C', substr($all, $p, 1));		# root
	$p += 1 + unpack('C', substr($all, $p, 1));		# base
	my $nfiles = unpack('V', substr($all, $p, 4));
	$p += 4 + 21 * $nfiles;
	my $nsegs = unpack('V', substr($all, $p, 4));
	die "manifest has $nsegs segments" if $n >= $nsegs;
	$p += 4 + 16 * $n + 8;
	substr($all, $p, 8) = "\0" x 8;
	substr($all, length($all) - 4, 4) =
	  pack('V', crc32c(substr($all, 0, length($all) - 4)));
	seek($fh, 0, 0);
	print $fh $all;
	close($fh);
	return;
}
