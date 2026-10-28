# Copyright (c) 2026, PostgreSQL Global Development Group

# A hard reset to a commit kept with a frozen root asks of each relation
# whether its pages changed since the root.  The question is answered at
# the first changed page found, so the map's nodes it reads don't grow
# with the working set the reset discards: a reset discarding 90% of a
# table's rows reads no more of them for it than one discarding 10%, but
# for the one walk that rebuilds the visibility map of the pages changed.
# Counted in bmap_changed_nodes_read, on the branch the commit was made on
# (a restore of its own storage) and on another branch (a share of its
# pages).  The rows are the commit's afterwards, none written.
#
# The updates are HOT (fillfactor 50), so the primary key's pages are the
# root's still and the table's own are the ones asked about.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('reset_changed_walk');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
fsync = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $nrows = 60000;
sql(q{ALTER DATABASE postgres SET prolly.root_interval = 0;
	ALTER DATABASE postgres SET prolly.root_interval_commits = 1});
sql(qq{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, rev int NOT NULL, payload text NOT NULL)
	  WITH (fillfactor = 50);
	INSERT INTO app.t SELECT g, 0, repeat(md5(g::text), 12)
	  FROM generate_series(1, $nrows) g;
	SELECT dolt_commit('-Am', 'load') IS NOT NULL});
my $head = sql(q{SELECT dolt_hashof('HEAD')});
my $digest = q{SELECT md5(string_agg(id || ':' || rev, ',' ORDER BY id)) FROM app.t};
my $at_head = sql($digest);

# (the map's leaves the table's pages take: a walk of every changed page
# reads about as many as it changed)
my $blocks = sql(q{SELECT pg_relation_size('app.t') / 8192});
ok($blocks > 4 * 509, "the table spans several of its map's leaves ($blocks blocks)");

my @names = qw(bmap_changed_nodes_read chunk_file_reads apply_rows
  moves_root_kept move_tables_written);
my $names = join(', ', map { "'$_'" } @names);
sub counters
{
	my %c = split /[|\n]/,
	  sql(qq{SELECT name, value FROM pg_stat_prolly WHERE name IN ($names)});
	$c{$_} //= 0 for @names;
	return \%c;
}

# branch $branch (made of main's head unless it's main) with the first
# $pct% of the rows updated, then hard reset to the head's commit: what it
# counted
my $n = 0;
sub dirty_reset
{
	my ($branch, $pct) = @_;
	my $co = "SELECT dolt_checkout('$branch') IS NOT NULL;";

	sql(qq{SELECT dolt_branch('$branch', '$head') IS NOT NULL}) if $branch ne 'main';
	sql(qq{$co UPDATE app.t SET rev = rev + 1 WHERE id <= $nrows * $pct / 100});
	my $before = counters();
	sql(qq{$co SELECT dolt_reset('--hard', '$head') IS NOT NULL;
		SELECT pg_stat_force_next_flush()});
	my $after = counters();
	my %d = map { $_ => $after->{$_} - $before->{$_} } @names;
	my @rows = split /\n/, sql(qq{$co $digest});
	is($rows[-1], $at_head, "$branch, $pct% dirty: the rows are the commit's");
	is($d{moves_root_kept}, 1, "$branch, $pct% dirty: the reset found the kept root");
	is($d{apply_rows} + $d{move_tables_written}, 0,
		"$branch, $pct% dirty: nothing written");
	return \%d;
}

for my $branch ('main', 'other')
{
	my $d10 = dirty_reset($branch eq 'main' ? 'main' : "${branch}10", 10);
	my $d90 = dirty_reset($branch eq 'main' ? 'main' : "${branch}90", 90);
	my $grown = $d90->{bmap_changed_nodes_read} - $d10->{bmap_changed_nodes_read};

	# (one walk of the changed pages, the visibility map's, grows by the
	# leaves 80% more of the table's pages take, a few more for their
	# parents; every question of whether a relation changed is answered
	# at the first changed page, adding none)
	my $one_walk = int(0.8 * $blocks / 509) + 3;
	note("$branch: changed map nodes read at 10% $d10->{bmap_changed_nodes_read}, "
		  . "at 90% $d90->{bmap_changed_nodes_read}, one walk's growth at most $one_walk");
	ok($grown <= $one_walk,
		"$branch: the map's nodes read grow by one walk's at most ($grown)");
	ok($d90->{chunk_file_reads} <= $d10->{chunk_file_reads} + 2,
		"$branch: the chunks read don't grow with the working set");
}

$node->stop;
done_testing();
