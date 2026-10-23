# Copyright (c) 2026, PostgreSQL Global Development Group

package ProllyTest;

use strict;
use warnings FATAL => 'all';
use Exporter 'import';
use Cwd qw(abs_path);
use File::Path qw(make_path);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our @EXPORT = qw(setup_target check_target run_requirements);

sub setup_target
{
	my ($node, $db, $am) = @_;
	$am =~ /\A(?:heap|prolly)\z/
	  or BAIL_OUT("unknown requirements target: $am");
	$node->safe_psql($db,
		"ALTER DATABASE \"$db\" SET default_table_access_method = '$am'");
	if ($am eq 'prolly')
	{
		$node->safe_psql($db,
			'DROP SCHEMA public; CREATE SCHEMA public VERSIONED');
	}
	$node->safe_psql($db, 'CREATE SCHEMA extensions');
	$node->safe_psql($db,
		"ALTER DATABASE \"$db\" SET search_path = public, extensions");
	check_target($node, $db, $am);
}

sub check_target
{
	my ($node, $db, $am) = @_;
	$node->safe_psql($db,
		'CREATE TABLE target_canary (id int PRIMARY KEY, payload text)');
	my $actual = $node->safe_psql($db, q{
		SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
		WHERE c.oid = 'target_canary'::regclass});
	$actual eq $am or die "requested $am, canary uses $actual\n";
	if ($am eq 'prolly')
	{
		my $paths = $node->safe_psql($db, q{
			SELECT pg_relation_filepath(oid) FROM pg_class
			WHERE oid = 'target_canary'::regclass
			   OR oid = 'target_canary_pkey'::regclass
			   OR oid = (SELECT reltoastrelid FROM pg_class
						 WHERE oid = 'target_canary'::regclass)});
		my @paths = split /\n/, $paths;
		@paths == 3 or BAIL_OUT('missing table, index or TOAST canary');
		for my $path (@paths)
		{
			-f $node->data_dir . "/${path}_bmap"
			  or BAIL_OUT("canary storage is not block-mapped: $path");
		}
	}
	$node->safe_psql($db, 'DROP TABLE target_canary');
	pass("$db uses $am storage");
}

sub run_requirements
{
	my ($kind) = @_;
	my $am = $ENV{PROLLY_AM} // 'prolly';
	my $node = PostgreSQL::Test::Cluster->new($kind);
	$node->init;
	$node->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
	$node->start;
	$node->safe_psql('postgres', 'CREATE DATABASE regression TEMPLATE template0');
	setup_target($node, 'regression', $am);
	my $out = "$PostgreSQL::Test::Utils::tmp_check/$kind";
	make_path($out);
	my $runner = $kind eq 'isolation'
	  ? "$ENV{top_builddir}/src/test/isolation/pg_isolation_regress"
	  : $ENV{PG_REGRESS};
	my $rc = system($runner, '--use-existing', '--bindir=',
		'--host=' . $node->host, '--port=' . $node->port,
		'--dbname=regression', '--inputdir=' . abs_path('.'),
		"--outputdir=$out", "--schedule=${kind}_schedule");
	diag(slurp_file("$out/regression.diffs"))
	  if $rc && -f "$out/regression.diffs";
	is($rc, 0, "$kind matches heap on $am");
	# (an extension's own tables, such as PostGIS's spatial_ref_sys, are
	# shared heap tables wherever they live)
	my $wrong = $node->safe_psql('regression', qq{
		SELECT c.relname, a.amname FROM pg_class c
		JOIN pg_am a ON a.oid = c.relam
		WHERE c.relnamespace = 'public'::regnamespace
		AND c.relkind IN ('r', 'm') AND a.amname <> '$am'
		AND NOT EXISTS (SELECT FROM pg_depend d
			WHERE d.classid = 'pg_class'::regclass AND d.objid = c.oid
			AND d.deptype = 'e')});
	is($wrong, '', 'remaining tables use the requested engine');
	$node->stop;
}

1;
