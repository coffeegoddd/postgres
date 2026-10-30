# Copyright (c) 2026, PostgreSQL Global Development Group

# A module loaded through shared_preload_libraries registers its
# implementations in every backend, so each session sees them without
# loading the module itself.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('preloaded');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_prolly'");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

is( $node->safe_psql(
		'postgres',
		"SELECT string_agg(implementation || ' ' || test || ' ' || result, ', '
				ORDER BY implementation, test)
		 FROM test_prolly_conformance('test_checksum')"),
	'drops_last_byte detects_changed_byte failed, '
	  . 'drops_last_byte detects_changed_length failed, '
	  . 'drops_last_byte deterministic ok, '
	  . 'drops_last_byte registered ok, '
	  . 'hash_bytes detects_changed_byte ok, '
	  . 'hash_bytes detects_changed_length ok, '
	  . 'hash_bytes deterministic ok, '
	  . 'hash_bytes registered ok',
	'preloaded implementations are registered in a new session');

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM test_prolly_unit_tests() WHERE result <> 'ok'"),
	'0',
	'unit tests pass in a server with the module preloaded');

$node->stop;
done_testing();
