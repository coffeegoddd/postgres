# Copyright (c) 2026, PostgreSQL Global Development Group

# A journal record whose CRC holds but whose compressed chunk doesn't
# decode is corrupt: a collection that takes its chunk for a root (here: in
# a segment a logical slot may still decode) refuses it, rather than skip
# it and drop what it names; and reading it is an error.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('corrupt_compressed');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nwal_level = logical");
$node->start;

if ($node->safe_psql('postgres',
		q{SELECT 'lz4' = ANY(enumvals) FROM pg_settings WHERE name = 'default_toast_compression'}
	) ne 't')
{
	plan skip_all => 'the server is built without LZ4';
}

$node->safe_psql('postgres', 'CREATE DATABASE cc');
$node->safe_psql('cc', 'CREATE EXTENSION test_prolly');
$node->safe_psql('cc',
	q{SELECT pg_create_logical_replication_slot('cc_slot', 'pgoutput')});
my $hash = $node->safe_psql('cc',
	q{SELECT test_chunk_put(convert_to(repeat('y', 50000), 'UTF8'))});
$node->safe_psql('cc', 'SELECT test_chunk_seal()');
my $oid = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'cc'");
my $journal = $node->data_dir . "/base/$oid/prolly_journal";

# CRC-32C (Castagnoli), as a record's last four bytes are of its kind and
# payload.
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

# The record: its length, its kind (a chunk, or one with its xid), the
# hash's length and the hash, then the codec (LZ4: 1), the chunk's length
# and the compressed bytes, then the CRC.  Its stated length is made one
# more, the CRC made again.
$node->stop;
my $all = slurp_file($journal);
my $hbytes = pack('H*', $hash);
my $at = index($all, pack('C', length($hbytes)) . $hbytes);
ok($at > 0, 'the chunk is in the journal');
my $start = unpack('C', substr($all, $at - 1, 1)) == 1 ? $at - 5 : $at - 9;
my $len = unpack('V', substr($all, $start, 4));
my $codec_at = $at + 1 + length($hbytes);
is(unpack('C', substr($all, $codec_at, 1)), 1, 'it is stored compressed');
my $rawlen = unpack('V', substr($all, $codec_at + 1, 4));
is($rawlen, 50000, '... with its length');
substr($all, $codec_at + 1, 4) = pack('V', $rawlen + 1);
substr($all, $start + $len - 4, 4) =
  pack('V', crc32c(substr($all, $start + 4, $len - 8)));
open(my $fh, '>:raw', $journal) or die "open $journal: $!";
print $fh $all;
close($fh);
$node->start;

my ($ret, $stdout, $stderr) =
  $node->psql('cc', 'SELECT * FROM test_chunk_gc(false)');
isnt($ret, 0, 'a collection refuses the record');
like($stderr, qr/corrupt chunk record/, '... as corrupt');

($ret, $stdout, $stderr) =
  $node->psql('cc', "SELECT length(test_chunk_get('$hash'))");
like($stderr, qr/corrupt chunk record/, 'reading it is an error');

$node->safe_psql('cc', q{SELECT pg_drop_replication_slot('cc_slot')});
$node->stop;
done_testing();
