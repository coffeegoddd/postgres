# Copyright (c) 2026, PostgreSQL Global Development Group

# A transaction gets the same few refs entries many times (its branch's
# head, merge state and names, the writer settings), and every get is a
# descent of the refs tree, whose levels grow with the roots kept in the
# history window.  A get that resolves to the refs root an earlier one read
# is answered without a descent, and a backend's own commits carry what it
# read to the root they make.  So a run of one-row transactions, each
# committed with dolt.transaction_commit, reads about as many of the refs'
# and commits' trees' nodes (refs_nodes_read) after two thousand commits as
# after a few: the descents left are those of the commit's own puts.
#
# The counts are of one session's transactions, so per transaction they
# are deltas of the database's counters with nothing else running.  A root
# is kept every 100 commits; a transaction that keeps one reads many more,
# so each is judged by the median of a run of them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('refs_gets_per_transaction');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
fsync = off
prolly.root_interval = 0
prolly.root_interval_commits = 100
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, payload text NOT NULL);
	INSERT INTO app.t SELECT g, md5(g::text) FROM generate_series(1, 10000) g;
	SELECT dolt_commit('-Am', 'load') IS NOT NULL;
	SELECT dolt_writer_mode('owned');
	VACUUM app.t});

# n one-row transactions, from the update numbered from on, each committed,
# in one session; the refs nodes each read, in order.
sub run
{
	my ($from, $n) = @_;
	my $script = "SET dolt.transaction_commit = on;\n"
	  . "CREATE TEMP TABLE m (i int, refs bigint);\n";
	for my $i ($from .. $from + $n)
	{
		my $id = $i * 7919 % 10000 + 1;
		$script .= "UPDATE app.t SET payload = md5(payload) WHERE id = $id;\n"
		  . "SELECT pg_stat_force_next_flush();\n"
		  . "INSERT INTO m SELECT $i, value FROM pg_stat_prolly "
		  . "WHERE name = 'refs_nodes_read';\n";
	}
	$script .= "SELECT string_agg((refs - lr)::text, ' ' ORDER BY i) FROM "
	  . "(SELECT i, refs, lag(refs) OVER (ORDER BY i) lr FROM m) s "
	  . "WHERE lr IS NOT NULL;\n";
	my $out = $node->safe_psql('postgres', $script);
	my @lines = grep { /^\d+( \d+)*$/ } split /\n/, $out;
	return split / /, $lines[-1];
}

sub median
{
	my @s = sort { $a <=> $b } @_;
	return $s[ int(@s / 2) ];
}

my @first = run(0, 20);
my $first = median(@first);
note "first transactions' refs_nodes_read: @first";

# (to about 2000 commits, twenty roots kept)
run(21, 1960);
my @last = run(1982, 20);
my $last = median(@last);
note "later transactions' refs_nodes_read: @last";

# Each transaction gets about fifteen entries, most of them its commit
# didn't change; with a descent for each, a level more of the refs tree
# took each from about 12 nodes to about 33.
cmp_ok($first, "<=", 9,
	"a transaction's refs reads are its commit's, not a descent per get ($first)");
cmp_ok($last, "<=", 16,
	"after two thousand commits they grow by the commit's own descents ($first -> $last)");

$node->stop;
done_testing();
