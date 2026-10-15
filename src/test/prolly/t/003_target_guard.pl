# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use ProllyTest;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('target_guard');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method = 'prolly'");
$node->start;

eval { check_target($node, 'postgres', 'prolly'); };
like($@, qr/requested prolly, canary uses heap/,
	'the guard rejects fallback to heap outside a versioned schema');
$node->safe_psql('postgres', 'DROP TABLE target_canary');
setup_target($node, 'postgres', 'prolly');
$node->stop;
done_testing();
