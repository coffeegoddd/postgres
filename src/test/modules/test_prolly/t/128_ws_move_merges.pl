# Copyright (c) 2026, PostgreSQL Global Development Group

# The merge scenarios of t/099, each that merges with no conflicts applied in place: ours moved to
# the root the merge made.  After the move the tables hold that root and capture has it, nothing a
# generated column calls ran, and what the scenario checks of the merged tables holds of ours, as
# what it says to resolve then clears.  (As in t/099, the schema a file calls rt is each side's
# own, and what a function's body names is by its path.)

use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move_merges');
$node->init(allows_streaming => 'logical');

# (the roots moved to are merged, and no commit holds them: no freer takes their chunks)
$node->append_conf('postgresql.conf',
	"autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\nchunk_free_in_background = off\n");
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

sub between
{
	my ($out, $tag) = @_;
	my ($got) = $out =~ /=====$tag\n(.*?)\n?=====end/s;
	return $got;
}

my ($i, $moved) = (0, 0);
for my $dir (map { dirname(__FILE__) . "/$_" } qw(ws_merge ws_merge_generated ws_merge_trigger ws_merge_dolt))
{
my $shared = -e "$dir/base.sql" ? slurp_file("$dir/base.sql") : undef;
for my $path (sort grep { basename($_) ne 'base.sql' } glob("$dir/*.sql"))
{
	$i++;
	my $name = basename($dir) . ' ' . basename($path, '.sql');
	my $s = sections($path);
	next if $s->{expect} =~ /^conflict /m;
	$moved++;
	my $base = $s->{base} // $shared;
	my @checks = map { /^check (.*) => (.*)$/ ? ([ $1, $2 ]) : () } split /\n/, $s->{expect};
	my @after = map { /^check (.*) => (.*)$/ ? ([ $1, $2 ]) : die "$name: what is this: $_" }
	  grep { /\S/ } split /\n/, $s->{after} // '';
	my ($b, $o, $t) = ("b$i", "o$i", "t$i");

	my $script = qq{
	SET client_min_messages = warning;
	SELECT test_ws_use_history_store() IS NULL AS on_history \\gset
	CREATE SCHEMA $b VERSIONED; ALTER SCHEMA $b OWNER TO g_scenario;
	CREATE SCHEMA $o VERSIONED; ALTER SCHEMA $o OWNER TO g_scenario;
	CREATE SCHEMA $t VERSIONED; ALTER SCHEMA $t OWNER TO g_scenario;
	SET ROLE g_scenario;
	SET search_path = $b;
} . in_schema($base, $b) . "SET search_path = $o;\n" . in_schema($base, $o) . in_schema($s->{ours}, $o)
	  . "SET search_path = $t;\n" . in_schema($base, $t) . in_schema($s->{theirs}, $t) . qq{
	RESET ROLE; SET search_path = public;
	SELECT test_ws_root_capture('$b') AS rb \\gset
	SELECT test_ws_root_capture('$o') AS ro \\gset
	SELECT test_ws_root_capture('$t') AS rt_ \\gset
	SELECT split_part(test_ws_merge_checked(:'rb', :'ro', :'rt_'), E'\\n', 1) AS m \\gset
	DROP SCHEMA $b, $t CASCADE;
	SELECT last_value AS calls FROM public.vc_gen_calls \\gset
	SELECT test_ws_move_prepare('$o', :'m') IS NULL AS prepared \\gset
	SELECT test_ws_move('$o', :'m') AS stats \\gset
	\\echo =====stats
	\\echo :stats
	\\echo =====end
	\\echo =====calls
	SELECT last_value = :calls FROM public.vc_gen_calls;
	\\echo =====end
	\\echo =====same
	SELECT test_ws_root_capture('$o') = :'m';
	\\echo =====end
	\\echo =====captured
	SELECT test_capture_barrier('$o') = :'m';
	\\echo =====end
};
	my $k = 0;
	for my $check (@checks)
	{
		$k++;
		$script .= "\\echo =====check$k\n" . in_schema($check->[0], $o) . ";\n\\echo =====end\n";
	}
	if (defined $s->{resolve})
	{
		$script .= in_schema($s->{resolve}, $o);
		$script .= qq{\\echo =====left\nSELECT test_ws_viol_resolve('$o');\n\\echo =====end\n};
		$k = 0;
		for my $check (@after)
		{
			$k++;
			$script .= "\\echo =====after$k\n" . in_schema($check->[0], $o) . ";\n\\echo =====end\n";
		}
	}
	$script .= "RESET client_min_messages; SET client_min_messages = warning;\nDROP SCHEMA IF EXISTS $b, $o, $t CASCADE;\n";
	my ($rc, $out, $err) = $node->psql('postgres', $script);
	is($rc, 0, "$name: runs") or diag($err);
	note("$name: " . (between($out, 'stats') // ''));
	is(between($out, 'same') // '', 't', "$name: moved, the tables hold the root the merge made");
	is(between($out, 'captured') // '', 't', "$name: and capture has it");
	is(between($out, 'calls') // '', 't', "$name: moved, computing nothing");
	$k = 0;
	for my $check (@checks)
	{
		$k++;
		is(between($out, "check$k") // '', in_schema($check->[1], $o), "$name: $check->[0]");
	}
	if (defined $s->{resolve})
	{
		is(between($out, 'left') // '', '0', "$name: resolved");
		$k = 0;
		for my $check (@after)
		{
			$k++;
			is(between($out, "after$k") // '', in_schema($check->[1], $o), "$name, resolved: $check->[0]");
		}
	}
}
}

is($i, 153, 'every scenario was read');
cmp_ok($moved, '>=', 100, "each with no conflicts was moved: $moved");

$node->stop;
done_testing();
