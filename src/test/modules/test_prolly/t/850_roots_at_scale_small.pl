# Copyright (c) 2026, PostgreSQL Global Development Group

# Kept roots at scale, small: the work each operation does (a commit, a
# reset to a kept commit and back, one to a commit without a root, the
# retention and freezing passes, a page sweep, VACUUM, the roots' slot
# storage) is the same, within the trees' depth, with about 200 and about
# 2,000 kept roots of every commit.  The same checks at 10,000 and
# 1,000,000 are t/851's (PG_TEST_EXTRA=prolly_scale).  Counters only.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;
use RootsAtScale;

my $node = RootsAtScale::node('roots_at_scale_small');
RootsAtScale::setup($node, 4);

my $few = RootsAtScale::grow($node, 200);
my $small = RootsAtScale::measure($node);
RootsAtScale::report("$few commits:", $small);
my $many = RootsAtScale::grow($node, 2000);
my $big = RootsAtScale::measure($node);
RootsAtScale::report("$many commits:", $big);

cmp_ok($many, '>=', 10 * $few * 0.9, "ten times the kept commits ($few, then $many)");
for my $m ($small, $big)
{
	cmp_ok($m->{reset_back}{relations_restored}, '>', 0, 'the reset back restored');
	is($m->{reset_back}{apply_rows}, 0, 'writing no row');
	cmp_ok($m->{reset_forth}{relations_restored}, '>', 0, 'and forth');
}
RootsAtScale::check_flat($small, $big, "$few and $many commits");

$node->stop;
done_testing();
