# Copyright (c) 2026, PostgreSQL Global Development Group

# A chunk index records the format of the chunks it names as it is made;
# pg_dump makes it again under that format (setting chunk_format before
# it, and resetting it after), so one restored names its chunks as the
# one dumped did.  (The test format is the test module's: preloaded, so
# the restore's session has it.)
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('chunk_format_dump');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'test_prolly'
chunk_free_in_background = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE src');
$node->safe_psql('postgres', 'CREATE DATABASE dst');
$node->safe_psql('src', q{
	CREATE EXTENSION test_prolly;
	SET chunk_format = leaf;
	CREATE TABLE leaf_log () USING prolly_chunks;
	CREATE TABLE leaf_idx () USING prolly_chunk_index;
	SET chunk_format = test;
	CREATE TABLE test_log () USING prolly_chunks;
	CREATE TABLE test_idx () USING prolly_chunk_index;
	RESET chunk_format;
	CREATE TABLE engine_idx () USING prolly_chunk_index;
});
my $formats = q{SELECT string_agg(pg_chunk_index_format(c::regclass), ' ' ORDER BY c)
	FROM unnest(ARRAY['engine_idx', 'leaf_idx', 'test_idx']) c};
is($node->safe_psql('src', $formats), 'engine leaf test', 'made in three formats');

my $dump = $node->basedir . '/src.sql';
$node->command_ok([ 'pg_dump', '--file' => $dump, '--dbname' => $node->connstr('src') ],
	'dumped');
my $text = slurp_file($dump);
like($text, qr/SET chunk_format = 'leaf';\nCREATE TABLE public\.leaf_idx/,
	'the dump makes the leaf index under its format');
like($text, qr/SET chunk_format = 'test';\nCREATE TABLE public\.test_idx/,
	'and the test one under its');
like($text, qr/CREATE TABLE public\.test_idx \(\n\);\nRESET chunk_format;/,
	'and resets the setting after');

$node->command_ok(
	[ 'psql', '--no-psqlrc', '--set' => 'ON_ERROR_STOP=1', '--file' => $dump,
	  '--dbname' => $node->connstr('dst') ],
	'restored');
is($node->safe_psql('dst', $formats), 'engine leaf test', 'restored in the three formats');

# a chunk naming one not there: refused by the test format's index, taken
# by the leaf one's, which names none
my ($ret, $out, $err) = $node->psql('dst', q{
	SELECT test_chunk_put('test_log', 'test_idx', substr(sha256('\x5201' || substr(sha256('\x01'), 1, 20)), 1, 20),
		'\x5201' || substr(sha256('\x01'), 1, 20))});
like($err, qr/which the store does not have/, 'the test index restored reads by its format');
is($node->safe_psql('dst', q{
	SELECT test_chunk_put('leaf_log', 'leaf_idx', substr(sha256('\x5201' || substr(sha256('\x01'), 1, 20)), 1, 20),
		'\x5201' || substr(sha256('\x01'), 1, 20))}), 't',
	'and the leaf one by its');

$node->stop;
done_testing();
