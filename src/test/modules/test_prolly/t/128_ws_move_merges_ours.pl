# Copyright (c) 2026, PostgreSQL Global Development Group

# The merge scenarios with no conflicts, ours moved in place to the root each merge made
# (ws_move_merges.pm).

use strict;
use warnings FATAL => 'all';
use File::Basename;
use File::Spec;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our $side = 'ours';
our $changed_moves = 109;
my $run = File::Spec->rel2abs(dirname(__FILE__)) . '/ws_move_merges.pm';
defined(do $run) or die "$run: " . ($@ || $!);
