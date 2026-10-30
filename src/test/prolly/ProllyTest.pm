
# Copyright (c) 2026, PostgreSQL Global Development Group

=pod

=head1 NAME

ProllyTest - TAP helpers for the version-control engine's requirements suite

=head1 SYNOPSIS

  use ProllyTest;

  my $node = ProllyTest::new_node('main');
  $node->start;

=head1 DESCRIPTION

Every server a requirements test starts must run on the table access method
under test, which the PROLLY_AM environment variable names (heap if it is
unset).  new_node makes a cluster with the settings in am/<PROLLY_AM>.conf
next to this module, the same settings the regression and isolation groups
use.

=cut

package ProllyTest;

use strict;
use warnings FATAL => 'all';

use File::Basename qw(dirname);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

=pod

=over

=item ProllyTest::table_am()

The name of the table access method under test.

=cut

sub table_am
{
	return $ENV{PROLLY_AM} // 'heap';
}

=pod

=item ProllyTest::new_node(name, %init_params)

A new, initialized but not started, cluster whose new tables go on the
access method under test.  %init_params are passed to init.

=cut

sub new_node
{
	my ($name, %init_params) = @_;
	my $conf = dirname(__FILE__) . '/am/' . table_am() . '.conf';

	die "there are no settings for table access method \"" . table_am() . "\": $conf does not exist"
	  unless -f $conf;

	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init(%init_params);
	$node->append_conf('postgresql.conf', slurp_file($conf));
	return $node;
}

=pod

=back

=cut

1;
