# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use ProllyTest;
use ProllyVC;
use Test::More;

my $am = $ENV{PROLLY_AM} // 'prolly_d1';
my $node = PostgreSQL::Test::Cluster->new('vc_target');
$node->init;
$node->start;
setup_target($node, 'postgres', $am);
my $checks = eval { check_vc_target($node, 'postgres', $am) };
my $error = $@;
ok(!$error, "$am commits, branches and restores its actual table data");
diag($error) if $error;
note("$checks version-control invariants verified") if defined $checks;
$node->stop;
done_testing();
