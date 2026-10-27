# Copyright (c) 2026, PostgreSQL Global Development Group

# Kept roots at scale: t/850's checks between about 10,000 and about
# 1,000,000 kept roots of every commit (PROLLY_SCALE_COMMITS for another
# bigger size), under a time window that holds them all.  Long (the
# commits are made one at a time): run only with PG_TEST_EXTRA containing
# prolly_scale.  Counters only.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;
use RootsAtScale;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bprolly_scale\b/)
{
	plan skip_all => 'Test prolly_scale not enabled in PG_TEST_EXTRA';
}
my $target = $ENV{PROLLY_SCALE_COMMITS} // 1000000;

my $node = RootsAtScale::node('roots_at_scale');
RootsAtScale::setup($node, 4);

my $few = RootsAtScale::grow($node, 10000);
my $small = RootsAtScale::measure($node);
RootsAtScale::report("$few commits:", $small);
my $many = RootsAtScale::grow($node, $target);
my $big = RootsAtScale::measure($node);
RootsAtScale::report("$many commits:", $big);

cmp_ok($many, '>=', $target * 0.99, "about $target kept commits ($many)");
cmp_ok($big->{reset_back}{relations_restored}, '>', 0, 'the reset back restored');
is($big->{reset_back}{apply_rows}, 0, 'writing no row');
RootsAtScale::check_flat($small, $big, "$few and $many commits");

$node->stop;
done_testing();
