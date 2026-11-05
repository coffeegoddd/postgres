# Copyright (c) 2026, PostgreSQL Global Development Group

# Merge scenarios, each of a file in the directories here: ours and theirs, each from one base, merged
# and checked.  What each expects is exact: its conflicts, or its violations, or, if it is
# clean, what its merged root says once restored.  The schema a file calls rt is each
# side's own here, and what a function's body names is by its path, as a body is the same
# text on each side only then.  Materialized views restore with no data.  What a file says
# to resolve waits for a root with violations to be restored.

use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('scenarios');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$ENV{PGOPTIONS} = "-c catalog_branch=$branch";
$node->safe_psql('postgres', 'CREATE ROLE g_scenario NOLOGIN TRAVELS');
$node->safe_psql('postgres', 'CREATE SEQUENCE public.vc_gen_calls; GRANT ALL ON SEQUENCE public.vc_gen_calls TO PUBLIC');

sub in_schema
{
	my ($sql, $schema) = @_;
	$sql =~ s/\brt\b/$schema/g;
	return $sql;
}

sub sections
{
	my ($file) = @_;
	my (%s, $at);
	for my $line (split /\n/, slurp_file($file))
	{
		if ($line =~ /^-- (ours|theirs|expect|resolve|after)$/) { $at = $1; $s{$at} = ''; next; }
		$s{$at} .= "$line\n" if defined $at;
	}
	return \%s;
}

my $i = 0;
for my $dir (map { dirname(__FILE__) . "/$_" } qw(ws_merge ws_merge_generated ws_merge_trigger))
{
my $base = slurp_file("$dir/base.sql");
for my $path (sort grep { basename($_) ne 'base.sql' } glob("$dir/*.sql"))
{
	$i++;
	my $name = basename($dir) . ' ' . basename($path, '.sql');
	my $s = sections($path);
	my @expect = grep { /\S/ } split /\n/, $s->{expect};
	my @want_conflicts = map { /^conflict (.*)$/ ? ($1) : () } @expect;
	my @want_violations = map { /^violation (\S+) (\S+) (.*)$/ ? ("$2 $1 $3") : () } @expect;
	my @checks = map { /^check (.*) => (.*)$/ ? ([ $1, $2 ]) : () } @expect;
	my ($b, $o, $t, $d) = ("b$i", "o$i", "t$i", "d$i");

	my $script = qq{
	SET client_min_messages = warning;
	CREATE SCHEMA $b VERSIONED; ALTER SCHEMA $b OWNER TO g_scenario;
	CREATE SCHEMA $o VERSIONED; ALTER SCHEMA $o OWNER TO g_scenario;
	CREATE SCHEMA $t VERSIONED; ALTER SCHEMA $t OWNER TO g_scenario;
	CREATE SCHEMA $d VERSIONED; ALTER SCHEMA $d OWNER TO g_scenario;
	SET ROLE g_scenario;
	SET search_path = $b;
} . in_schema($base, $b) . "SET search_path = $o;\n" . in_schema($base, $o) . in_schema($s->{ours}, $o)
	  . "SET search_path = $t;\n" . in_schema($base, $t) . in_schema($s->{theirs}, $t) . qq{
	RESET ROLE; SET search_path = public;
	SELECT test_ws_root_capture('$b') AS rb \\gset
	SELECT test_ws_root_capture('$o') AS ro \\gset
	SELECT test_ws_root_capture('$t') AS rt_ \\gset
	SELECT test_ws_merge_checked(:'rb', :'ro', :'rt_', ARRAY['$o', '$t', '$b']) AS merged \\gset
	\\echo =====merge
	\\echo :merged
	\\echo =====end
	SELECT split_part(:'merged', E'\\n', 1) AS m \\gset
	\\echo =====viol
	SELECT test_ws_violations(:'m');
	\\echo =====end
	};
	if (!@want_conflicts && !@want_violations)
	{
		$script .= qq{SELECT test_ws_root_restore(:'m', '$d') IS NULL AS restored \\gset\n};
		$script .= qq{\\echo =====same\nSELECT test_ws_root_capture('$d') = :'m';\n\\echo =====end\n};
		my $k = 0;
		for my $check (@checks)
		{
			$k++;
			$script .= "\\echo =====check$k\n" . in_schema($check->[0], $d) . ";\n\\echo =====end\n";
		}
	}
	# (a scenario's catalog checks see only what it restored)
	$script =~ s/(\\echo =====same\n)/DROP SCHEMA $b, $o, $t CASCADE;\n$1/;
	$script .= "RESET client_min_messages; SET client_min_messages = warning;\nDROP SCHEMA IF EXISTS $b, $o, $t, $d CASCADE;\n";
	my ($rc, $out, $err) = $node->psql('postgres', $script);
	is($rc, 0, "$name: runs") or diag($err);
	my ($merge) = $out =~ /=====merge\n(.*?)\n?=====end/s;
	my @conflicts = split /\n/, $merge // '';
	shift @conflicts;
	is_deeply([ sort @conflicts ], [ sort @want_conflicts ], "$name: its conflicts")
	  or diag(join("\n", 'it said:', @conflicts));
	next if @want_conflicts;
	my ($viol) = $out =~ /=====viol\n(.*?)\n?=====end/s;
	my @got = map { /^(?:foreign key|not null|\S+) (\S+ \S+ .*)$/ ? ($1) : () } split /\n/, $viol // '';
	is_deeply([ sort @got ], [ sort @want_violations ], "$name: its violations");
	next if @want_violations;
	my ($same) = $out =~ /=====same\n(.*?)\n?=====end/s;
	is($same // '', 't', "$name: restored, the root the merge made");
	my $k = 0;
	for my $check (@checks)
	{
		$k++;
		my ($got) = $out =~ /=====check$k\n(.*?)\n?=====end/s;
		is($got // '', in_schema($check->[1], $d), "$name: $check->[0]");
	}
}
}

$node->stop;
done_testing();
