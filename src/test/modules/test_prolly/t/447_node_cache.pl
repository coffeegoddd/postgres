# Copyright (c) 2026, PostgreSQL Global Development Group

# The shared node cache on a standby, and with the cache off.  A database
# dropped on the primary is forgotten by the standby's cache as its redo
# drops it: a database the primary then makes with the same OID has none
# of the dropped one's chunks, and a read of one on the standby finds
# nothing, as its store says.  With prolly_node_cache_size = 0 there is no
# cache: reads go to the files, and no hit or miss is counted.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node_cache_primary');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('node_cache_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

$node->safe_psql('postgres', 'CREATE DATABASE nc OID = 900002');
$node->safe_psql('nc', 'CREATE EXTENSION test_prolly');
my $h = $node->safe_psql('nc', q{SELECT test_chunk_put('standby oid reuse')});
$node->wait_for_replay_catchup($standby);

# (read three times on the standby: kept from the second read)
for my $i (1 .. 3)
{
	is($standby->safe_psql('nc', "SELECT test_chunk_get('$h') IS NOT NULL"),
		't', "the standby reads the chunk ($i)");
}

$node->safe_psql('postgres', 'DROP DATABASE nc');
$node->safe_psql('postgres', 'CREATE DATABASE nc OID = 900002');
$node->safe_psql('nc', 'CREATE EXTENSION test_prolly');
$node->wait_for_replay_catchup($standby);
is( $standby->safe_psql(
		'nc',
		"SELECT test_chunk_has('$h') || ' ' || (test_chunk_get('$h') IS NULL)"),
	'false true',
	'a database made with a dropped one\'s OID is given none of its chunks on the standby'
);

$standby->stop;

# With the cache off: reads from the files, and no hit or miss counted.
$node->append_conf('postgresql.conf', 'prolly_node_cache_size = 0');
$node->restart;
my $counts = q{SELECT string_agg(name || '=' || value, ' ' ORDER BY name)
	FROM pg_stat_prolly WHERE name IN ('node_cache_hits', 'node_cache_misses')};
$node->safe_psql('nc', 'SELECT pg_stat_reset_shared(\'prolly\')');
my $h2 = $node->safe_psql('nc', q{SELECT test_chunk_put('cache off')});
is( $node->safe_psql(
		'nc', qq{SELECT test_chunk_get('$h2') IS NOT NULL;
		SELECT test_chunk_get('$h2') IS NOT NULL;
		SELECT test_chunk_get('$h2') IS NOT NULL;
		SELECT pg_stat_force_next_flush()}),
	"t\nt\nt\n",
	'the cache off, chunks are read');
is($node->safe_psql('nc', $counts),
	'node_cache_hits=0 node_cache_misses=0',
	'the cache off, no hit or miss is counted');
cmp_ok(
	$node->safe_psql('nc',
		q{SELECT value FROM pg_stat_prolly WHERE name = 'chunk_file_reads'}),
	'>', 0, 'the cache off, the files are read');

$node->stop;
done_testing();
