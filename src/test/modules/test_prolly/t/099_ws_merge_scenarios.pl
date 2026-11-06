# Copyright (c) 2026, PostgreSQL Global Development Group

# Merge scenarios, each of a file in the directories here (ws_merge_dolt's are Dolt's own merge
# tests, in Postgres's terms, its expected results but where ws_merge.h says): ours and theirs, each from one base, merged
# and checked.  What each expects is exact: its conflicts, or its violations, or, if it is
# clean, what its merged root says once restored.  The schema a file calls rt is each
# side's own here, and what a function's body names is by its path, as a body is the same
# text on each side only then.  Materialized views restore with no data.  A root with
# violations restores too, capturing as itself, and what a file says to resolve must clear
# them.

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
		if ($line =~ /^-- (base|ours|theirs|expect|resolve|after)$/) { $at = $1; $s{$at} = ''; next; }
		$s{$at} .= "$line\n" if defined $at;
	}
	return \%s;
}

my $i = 0;
for my $dir (map { dirname(__FILE__) . "/$_" } qw(ws_merge ws_merge_generated ws_merge_trigger ws_merge_dolt))
{
my $shared = -e "$dir/base.sql" ? slurp_file("$dir/base.sql") : undef;
for my $path (sort grep { basename($_) ne 'base.sql' } glob("$dir/*.sql"))
{
	$i++;
	my $name = basename($dir) . ' ' . basename($path, '.sql');
	my $s = sections($path);
	my $base = $s->{base} // $shared;
	my @expect = grep { /\S/ } split /\n/, $s->{expect};
	my (@want_conflicts, @want_violations, @checks);
	for my $line (@expect)
	{
		if ($line =~ /^conflict (.*)$/) { push @want_conflicts, $1; }
		elsif ($line =~ /^violation (.*)$/) { push @want_violations, $1; }
		elsif ($line =~ /^check (.*) => (.*)$/) { push @checks, [ $1, $2 ]; }
		elsif ($line ne 'clean') { die "$name: what is this: $line"; }
	}
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
	SELECT test_ws_merge_checked(:'rb', :'ro', :'rt_') AS merged \\gset
	\\echo =====merge
	\\echo :merged
	\\echo =====end
	SELECT split_part(:'merged', E'\\n', 1) AS m \\gset
	\\echo =====viol
	SELECT test_ws_violations(:'m');
	\\echo =====end
	};
	my @after = map { /^check (.*) => (.*)$/ ? ([ $1, $2 ]) : die "$name: what is this: $_" }
	  grep { /\S/ } split /\n/, $s->{after} // '';
	if (!@want_conflicts)
	{
		$script .= qq{SELECT last_value AS calls FROM public.vc_gen_calls \\gset\n};
		$script .= qq{SELECT test_ws_root_restore(:'m', '$d') IS NULL AS restored \\gset\n};
		$script .= qq{\\echo =====calls\nSELECT last_value = :calls FROM public.vc_gen_calls;\n\\echo =====end\n};
		$script .= qq{\\echo =====same\nSELECT test_ws_root_capture('$d') = :'m';\n\\echo =====end\n};
		my $k = 0;
		for my $check (@checks)
		{
			$k++;
			$script .= "\\echo =====check$k\n" . in_schema($check->[0], $d) . ";\n\\echo =====end\n";
		}
		if (defined $s->{resolve})
		{
			$script .= in_schema($s->{resolve}, $d);
			$script .= qq{\\echo =====left\nSELECT test_ws_viol_resolve('$d');\n\\echo =====end\n};
			$script .= qq{\\echo =====cleared\nSELECT test_ws_violations(test_ws_root_capture('$d'));\n\\echo =====end\n};
			$k = 0;
			for my $check (@after)
			{
				$k++;
				$script .= "\\echo =====after$k\n" . in_schema($check->[0], $d) . ";\n\\echo =====end\n";
			}
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
	my @got = split /\n/, $viol // '';
	is_deeply([ sort @got ], [ sort @want_violations ], "$name: its violations");
	my ($same) = $out =~ /=====same\n(.*?)\n?=====end/s;
	is($same // '', 't', "$name: restored, the root the merge made");
	my ($calls) = $out =~ /=====calls\n(.*?)\n?=====end/s;
	is($calls // '', 't', "$name: restored, computing nothing");
	my $k = 0;
	for my $check (@checks)
	{
		$k++;
		my ($got) = $out =~ /=====check$k\n(.*?)\n?=====end/s;
		is($got // '', in_schema($check->[1], $d), "$name: $check->[0]");
	}
	if (defined $s->{resolve})
	{
		my ($left) = $out =~ /=====left\n(.*?)\n?=====end/s;
		is($left // '', '0', "$name: resolved");
		my ($cleared) = $out =~ /=====cleared\n(.*?)\n?=====end/s;
		is($cleared // 'none', '', "$name: captures with no violations");
		$k = 0;
		for my $check (@after)
		{
			$k++;
			my ($got) = $out =~ /=====after$k\n(.*?)\n?=====end/s;
			is($got // '', in_schema($check->[1], $d), "$name, resolved: $check->[0]");
		}
	}
}
}

is($i, 153, 'every scenario ran');

$node->stop;
done_testing();
