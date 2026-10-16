# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use ProllyTest;
use ProllyVC;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vc_target_guard');
$node->init;
$node->start;

# Each control uses a separate database, including its own refs and functions.
# Check a specific data invariant, never just "any failure": syntax errors or
# setup failures must not make a broken probe look like a working guard.
my @controls = (
	{
		name => 'heap is not versioned by successful dolt calls',
		am => 'heap',
		error => qr/VC target heap \[main isolated from feature rows\]: expected/,
	},
	{
		name => 'successful no-op reset is rejected',
		am => 'prolly_d1',
		mock => q{
			CREATE FUNCTION public.dolt_reset(VARIADIC text[]) RETURNS text[]
			LANGUAGE sql AS 'SELECT ARRAY[''0'']::text[]';
		},
		error => qr/VC target prolly_d1 \[restored feature HEAD rows\]: expected/,
	},
	{
		name => 'successful no-op checkout is rejected',
		am => 'prolly_d1',
		mock => q{
			CREATE FUNCTION public.dolt_checkout(VARIADIC text[]) RETURNS text[]
			LANGUAGE sql AS 'SELECT ARRAY[''0'', ''success'']::text[]';
		},
		error => qr/VC target prolly_d1 \[branch copy branch\]: expected/,
	},
	{
		name => 'a real old commit id cannot substitute for a data snapshot',
		am => 'prolly_d1',
		mock => q{
			SELECT dolt_commit('--allow-empty', '-m', 'empty before fixture');
			CREATE FUNCTION public.dolt_commit(VARIADIC text[]) RETURNS text[]
			LANGUAGE sql AS 'SELECT ARRAY[dolt_hashof(''HEAD'')]::text[]';
		},
		error => qr/VC target prolly_d1 \[branch copy rows\]: expected/,
	});

my $i = 0;
for my $control (@controls)
{
	subtest $control->{name} => sub {
		my $db = 'control' . ++$i;
		$node->safe_psql('postgres', "CREATE DATABASE $db TEMPLATE template0");
		setup_target($node, $db, $control->{am});
		if ($control->{mock})
		{
			$node->safe_psql($db, $control->{mock});
			# Make the deliberate public mocks precede the built-in functions.
			$node->safe_psql('postgres',
				"ALTER DATABASE $db SET search_path = public, pg_catalog, extensions");
		}
		eval { check_vc_target($node, $db, $control->{am}); };
		like($@, $control->{error}, 'the specific VC invariant rejects the target');
	};
}

$node->stop;
done_testing();
