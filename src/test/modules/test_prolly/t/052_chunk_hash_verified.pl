# Copyright (c) 2026, PostgreSQL Global Development Group

# A chunk's bytes are checked against its name as they are read: a byte
# changed on disk under a chunk (with no page checksums to catch it first)
# is an error, not a chunk.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('chunk_hash');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE cl () USING prolly_chunks;
	CREATE TABLE ci () USING prolly_chunk_index;
	SELECT test_chunk_put('cl', 'ci', substr(sha256(d), 1, 20), d)
	FROM (SELECT convert_to(repeat('A', 100), 'UTF8') AS d) s;
});
my $get = q{SELECT test_chunk_get('cl', 'ci', substr(sha256(convert_to(repeat('A', 100), 'UTF8')), 1, 20))
	= convert_to(repeat('A', 100), 'UTF8')};
is($node->safe_psql('postgres', $get), 't', 'the chunk reads back');

# where its 10th byte is in the log's file: a page holds its bytes after a
# 24-byte header and before an 8-byte special space, and the log's block 1
# holds its first
my $loc = $node->safe_psql('postgres',
	q{SELECT loc FROM test_chunk_index_entry('ci', substr(sha256(convert_to(repeat('A', 100), 'UTF8')), 1, 20))});
my $path = $node->data_dir . '/' . $node->safe_psql('postgres', q{SELECT pg_relation_filepath('cl')});
my $bs = $node->safe_psql('postgres', q{SELECT current_setting('block_size')});
my $area = $bs - 24 - 8;
my $at = (1 + int($loc / $area)) * $bs + 24 + ($loc % $area) + 10;
$node->stop;

open(my $fh, '+<', $path) or die "open $path: $!";
binmode $fh;
seek($fh, $at, 0) or die;
print $fh 'B';
close $fh;

$node->start;
my ($ret, $out, $err) = $node->psql('postgres', $get);
like($err, qr/stored chunk [0-9a-f]{40} does not match its hash.*DETAIL:  Its 100 bytes at \d+ in chunk log "cl"/s,
	'a byte changed under it is an error');
$node->stop;
done_testing();
