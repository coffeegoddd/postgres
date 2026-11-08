# Copyright (c) 2026, PostgreSQL Global Development Group

# What t/128 and t/129 run, with $side ours or theirs: the merge scenarios of t/099, each that
# merges with no conflicts applied in place, the side moved to the root the merge made.  After each
# move the tables hold that root and capture has it, with the violations the scenario expects;
# nothing a generated column calls ran; and what the scenario checks of the merged tables holds of
# the side moved, as what it says to resolve then clears.  (As in t/099, the schema a file calls rt
# is each side's own, and what a function's body names is by its path; a scenario's catalog checks
# see only the side moved.)

use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our $side;
our $changed_moves;

my $node = PostgreSQL::Test::Cluster->new("move_merges_$side");
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

# One scenario, the side moved being ours or theirs; whether the move had anything to change.
sub moved
{
	my ($name, $s, $base, $side, $i) = @_;
	my @want_violations = map { /^violation (.*)$/ ? ($1) : () } split /\n/, $s->{expect};
	# (a move refreshes a materialized view that held rows, where restore leaves each with none:
	# what a view holds is asked of each below, as the move has it)
	my @checks = grep { $_->[0] !~ /\bispopulated\b/ }
	  map { /^check (.*) => (.*)$/ ? ([ $1, $2 ]) : () } split /\n/, $s->{expect};
	my @after = map { /^check (.*) => (.*)$/ ? ([ $1, $2 ]) : die "$name: what is this: $_" }
	  grep { /\S/ } split /\n/, $s->{after} // '';
	my ($b, $o, $t) = ("b$i", "o$i", "t$i");
	my ($m, $other) = $side eq 'ours' ? ($o, $t) : ($t, $o);
	$name .= ", $side moved";

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
	SELECT test_ws_root_capture('$m') AS was \\gset
	SELECT test_ws_merge_checked(:'rb', :'ro', :'rt_') AS merged \\gset
	\\echo =====merge
	\\echo :merged
	\\echo =====end
	SELECT split_part(:'merged', E'\\n', 1) AS m \\gset
	DROP SCHEMA $b, $other CASCADE;
	SELECT coalesce(string_agg(matviewname || ':' || ispopulated, ',' ORDER BY matviewname), '') AS views
		FROM pg_matviews WHERE schemaname = '$m' \\gset
	SELECT last_value AS calls FROM public.vc_gen_calls \\gset
	SELECT test_ws_move_prepare('$m', :'m') IS NULL AS prepared \\gset
	SELECT test_ws_move('$m', :'m') AS stats \\gset
	\\echo =====stats
	\\echo :stats
	\\echo =====end
	\\echo =====changes
	SELECT :'m' <> :'was';
	\\echo =====end
	\\echo =====calls
	SELECT last_value = :calls FROM public.vc_gen_calls;
	\\echo =====end
	\\echo =====same
	SELECT test_ws_root_capture('$m') = :'m';
	\\echo =====end
	SELECT test_capture_barrier('$m') AS captured \\gset
	\\echo =====captured
	SELECT :'captured' = :'m';
	\\echo =====end
	\\echo =====viol
	SELECT test_ws_violations(:'captured');
	\\echo =====end
	\\echo =====views
	SELECT string_agg(matviewname || ':' || (ispopulated = ((matviewname || ':true') = ANY (string_to_array(:'views', ',')))),
		',' ORDER BY matviewname) FROM pg_matviews WHERE schemaname = '$m';
	\\echo =====end
};
	my $k = 0;
	for my $check (@checks)
	{
		$k++;
		$script .= "\\echo =====check$k\n" . in_schema($check->[0], $m) . ";\n\\echo =====end\n";
	}
	if (defined $s->{resolve})
	{
		$script .= in_schema($s->{resolve}, $m);
		$script .= qq{\\echo =====left\nSELECT test_ws_viol_resolve('$m');\n\\echo =====end\n};
		$script .= qq{\\echo =====cleared\nSELECT test_ws_violations(test_capture_barrier('$m'));\n\\echo =====end\n};
		$k = 0;
		for my $check (@after)
		{
			$k++;
			$script .= "\\echo =====after$k\n" . in_schema($check->[0], $m) . ";\n\\echo =====end\n";
		}
	}
	my ($rc, $out, $err) = $node->psql('postgres', $script);
	is($rc, 0, "$name: runs") or diag($err);

	# (what a scenario that stopped left is not for the next to see)
	$node->safe_psql('postgres', "SET client_min_messages = warning; DROP SCHEMA IF EXISTS $b, $o, $t CASCADE");
	my @conflicts = split /\n/, between($out, 'merge') // '';
	like(shift(@conflicts) // '', qr/^[0-9a-f]{40}$/, "$name: merged to a root");
	is_deeply(\@conflicts, [], "$name: with no conflicts");
	note("$name: " . (between($out, 'stats') // ''));
	is(between($out, 'same') // '', 't', "$name: the tables hold the root the merge made");
	is(between($out, 'captured') // '', 't', "$name: and capture has it");
	is_deeply([ sort split /\n/, between($out, 'viol') // '' ], [ sort @want_violations ], "$name: with its violations");
	is(between($out, 'calls') // '', 't', "$name: computing nothing");
	unlike(between($out, 'views') // '', qr/:f/, "$name: a materialized view holds rows if it held them, and only then");
	$k = 0;
	for my $check (@checks)
	{
		$k++;
		is(between($out, "check$k") // '', in_schema($check->[1], $m), "$name: $check->[0]");
	}
	if (defined $s->{resolve})
	{
		is(between($out, 'left') // '', '0', "$name: resolved");
		is(between($out, 'cleared') // 'none', '', "$name: captures with no violations");
		$k = 0;
		for my $check (@after)
		{
			$k++;
			is(between($out, "after$k") // '', in_schema($check->[1], $m), "$name, resolved: $check->[0]");
		}
	}
	return (between($out, 'changes') // '') eq 't';
}

my ($i, $scenarios, $changed) = (0, 0, 0);
for my $dir (map { dirname(__FILE__) . "/$_" } qw(ws_merge ws_merge_generated ws_merge_trigger ws_merge_dolt))
{
	my $shared = -e "$dir/base.sql" ? slurp_file("$dir/base.sql") : undef;
	for my $path (sort grep { basename($_) ne 'base.sql' } glob("$dir/*.sql"))
	{
		$i++;
		my $name = basename($dir) . ' ' . basename($path, '.sql');
		my $s = sections($path);
		next if $s->{expect} =~ /^conflict /m;
		$scenarios++;
		$changed++ if moved($name, $s, $s->{base} // $shared, $side, $i);
	}
}

is($i, 153, 'every scenario was read');
is($scenarios, 111, 'each with no conflicts was moved');
note("moves that had something to change: $changed");
is($changed, $changed_moves, 'and each that had something to change, did');

$node->stop;
done_testing();
