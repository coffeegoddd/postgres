# Copyright (c) 2026, PostgreSQL Global Development Group

# A chunk store records the format it is made in: the hash its chunks are
# named by (name and length) and the version of their layout, in each
# journal segment's header and in its manifest.  A server of another
# format, or one that reads another version of those files, refuses the
# store rather than misread it; put back as it was, the store reads again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('store_format');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

# a store with segment 0 alone, and one with a manifest (sealed once)
my %dir;
for my $db ('journal_only', 'manifested')
{
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, 'CREATE EXTENSION test_prolly');
	$node->safe_psql($db, q{SELECT test_chunk_put('format check')});
	my $oid = $node->safe_psql('postgres',
		"SELECT oid FROM pg_database WHERE datname = '$db'");
	$dir{$db} = $node->data_dir . "/base/$oid";
}
$node->safe_psql('manifested', 'SELECT test_chunk_seal()');
ok(-e "$dir{manifested}/prolly_manifest", 'a sealed store has a manifest');
ok(!-e "$dir{journal_only}/prolly_manifest", 'an unsealed one has none');

my $read = q{SELECT convert_from(test_chunk_get(test_content_hash('format check')), 'UTF8')};
is($node->safe_psql($_, $read), 'format check', "$_ reads") for sort keys %dir;

# Overwrite len bytes of a file at offset; returns what was there.
sub patch
{
	my ($path, $offset, $bytes) = @_;
	open(my $fh, '+<:raw', $path) or die "open $path: $!";
	seek($fh, $offset, 0);
	read($fh, my $old, length($bytes));
	seek($fh, $offset, 0);
	print $fh $bytes;
	close($fh);
	return $old;
}

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

# Patch a manifest as patch() does, its CRC made again.
sub patch_manifest
{
	my ($path, $offset, $bytes) = @_;
	my $old = patch($path, $offset, $bytes);
	open(my $fh, '+<:raw', $path) or die "open $path: $!";
	local $/;
	my $all = <$fh>;
	seek($fh, length($all) - 4, 0);
	print $fh pack('V', crc32c(substr($all, 0, length($all) - 4)));
	close($fh);
	return $old;
}

# The format in a segment's header: after the magic and the version, the
# hash's name (16 bytes, zero-padded), its length, the chunks' version.
my $sha256 = pack('a16 C', 'sha256', 32);
my $journal = "$dir{journal_only}/prolly_journal";

$node->stop;
my $was = patch($journal, 12, $sha256);
$node->start;
my ($ret, $stdout, $stderr) = $node->psql('journal_only', $read);
isnt($ret, 0, 'a journal of another hash is refused');
like($stderr, qr/is a chunk store journal of another format/, '... as another format');
like($stderr, qr/Its chunks are sha256 \(32 bytes\), chunk format \d+; this server's are blake3 \(20 bytes\)/,
	'... naming both');

$node->stop;
patch($journal, 12, $was);
my $version = patch($journal, 8, pack('V', 1));
$node->start;
($ret, $stdout, $stderr) = $node->psql('journal_only', $read);
like($stderr, qr/is a chunk store journal of version 1, not \d+/,
	'a journal of an older version is refused');

# A hash's name equal to this server's up to its zero, but not past it: a
# damaged header, refused (not read as if the file were open).
$node->stop;
patch($journal, 8, $version);
$was = patch($journal, 12, pack('a16', "blake3\0x"));
$node->start;
($ret, $stdout, $stderr) = $node->psql('journal_only', $read);
like($stderr, qr/has a damaged chunk journal header/,
	'a header with bytes past the hash name is refused');
($ret, $stdout, $stderr) = $node->psql('journal_only', $read);
like($stderr, qr/has a damaged chunk journal header/, '... each time');

# An older version's header shorter than this one's, holding no records:
# refused as that version, not read as an empty journal.
$node->stop;
patch($journal, 12, $was);
my $whole = slurp_file($journal);
open(my $fh, '>:raw', $journal) or die "open $journal: $!";
print $fh substr($whole, 0, 8) . pack('V V', 1, 0);
close($fh);
$node->start;
($ret, $stdout, $stderr) = $node->psql('journal_only', $read);
like($stderr, qr/is a chunk store journal of version 1, not \d+/,
	'a short journal of an older version is refused');

$node->stop;
open($fh, '>:raw', $journal) or die "open $journal: $!";
print $fh $whole;
close($fh);
$node->start;
is($node->safe_psql('journal_only', $read), 'format check',
	'put back, the journal reads again');

# The manifest: its magic, its version, then the format.
my $manifest = "$dir{manifested}/prolly_manifest";
$node->stop;
$was = patch_manifest($manifest, 12, $sha256);
$node->start;
($ret, $stdout, $stderr) = $node->psql('manifested', $read);
like($stderr, qr/is a chunk store manifest of another format/,
	'a manifest of another hash is refused');

$node->stop;
patch_manifest($manifest, 12, $was);
$version = patch_manifest($manifest, 8, pack('V', 3));
$node->start;
($ret, $stdout, $stderr) = $node->psql('manifested', $read);
like($stderr, qr/is a chunk store manifest of version 3, not \d+/,
	'a manifest of an older version is refused');

# An older version's manifest smaller than this version's least: refused
# as that version, not as no manifest.
$node->stop;
patch_manifest($manifest, 8, $version);
$whole = slurp_file($manifest);
open($fh, '>:raw', $manifest) or die "open $manifest: $!";
print $fh substr($whole, 0, 8) . pack('V', 3) . ("\0" x 45);
close($fh);
$node->start;
($ret, $stdout, $stderr) = $node->psql('manifested', $read);
like($stderr, qr/is a chunk store manifest of version 3, not \d+/,
	'a small manifest of an older version is refused as that version');
like($stderr, qr/HINT:  There is no conversion/, '... with no way to convert it');

$node->stop;
open($fh, '>:raw', $manifest) or die "open $manifest: $!";
print $fh $whole;
close($fh);
$node->start;
is($node->safe_psql('manifested', $read), 'format check',
	'put back, the manifest reads again');

$node->stop;
done_testing();
