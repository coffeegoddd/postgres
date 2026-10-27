# Copyright (c) 2026, PostgreSQL Global Development Group

# Kept roots at scale: a database whose branch keeps a root of every
# commit under a time window long enough for all of them, grown to a
# number of commits in one backend (a procedure committing each), and the
# work each operation does there, told by pg_stat_prolly's counters (never
# by time).  Shared by the scale tests: measure() at two sizes, then
# flat() for each operation whose work mustn't grow with the roots.

package RootsAtScale;

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# the counters each operation is measured by
our @COUNTERS = qw(refs_nodes_read refs_nodes_written kept_entries_read
  kept_entries_written kept_status_checks bmap_roots_pages_read
  bmap_refcount_reads relations_restored apply_rows);

# A node set up for it: no background work but what an operation does.
sub node
{
	my ($name) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
fsync = off
synchronous_commit = off
shared_buffers = 256MB
max_wal_size = 4GB
checkpoint_timeout = 1h
});
	$node->start;
	return $node;
}

sub sql { my ($node, $q) = @_; return $node->safe_psql('postgres', $q); }

# The database: $ntables versioned tables of a few rows, a first commit, and
# gen(n), n commits more, each changing one row of one table.
sub setup
{
	my ($node, $ntables) = @_;
	sql($node, q{ALTER DATABASE postgres SET prolly.history_window = '1000 years'});
	my $make = "CREATE EXTENSION test_prolly;\nCREATE SCHEMA app VERSIONED;\n";
	$make .= "CREATE TABLE app.t$_ (id int PRIMARY KEY, v int NOT NULL);\n"
	  . "INSERT INTO app.t$_ SELECT g, 0 FROM generate_series(1, 200) g;\n"
	  for (1 .. $ntables);
	sql($node, $make);
	sql($node, qq{SELECT dolt_commit('-Am', 'c0') IS NOT NULL;
	CREATE PROCEDURE gen(n int) LANGUAGE plpgsql AS \$\$
	BEGIN
		FOR i IN 1..n LOOP
			EXECUTE format('UPDATE app.t%s SET v = v + 1 WHERE id = %s',
						   i % $ntables + 1, i % 200 + 1);
			PERFORM dolt_commit('-am', 'g');
			COMMIT;
		END LOOP;
	END \$\$});
	return;
}

# The branch's commits: the kept ones (a root of every commit).
sub kept { my ($node) = @_; return sql($node, q{SELECT count(*) FROM test_kept_commits('main')}); }

# Grown to about $commits kept commits, in batches.
sub grow
{
	my ($node, $commits) = @_;
	my $have = kept($node);
	while ($have < $commits)
	{
		my $n = $commits - $have;
		$n = 50000 if $n > 50000;
		$node->safe_psql('postgres', "CALL gen($n)", timeout => 3 * 3600);
		$have = kept($node);
	}
	return $have;
}

sub counters
{
	my ($node) = @_;
	my $names = join(', ', map { "'$_'" } @COUNTERS);
	my %c = split /[|\n]/,
	  sql($node, qq{SELECT name, value FROM pg_stat_prolly WHERE name IN ($names)});
	$c{$_} //= 0 for @COUNTERS;
	return \%c;
}

# What $q did, counter by counter (run in a session of its own, whose
# counts are flushed as it ends).
sub counted
{
	my ($node, $q) = @_;
	my $before = counters($node);
	sql($node, $q);
	my $after = counters($node);
	return { map { $_ => $after->{$_} - $before->{$_} } @COUNTERS };
}

# Each operation's work at the database's size now: a commit; a reset to
# the commit before the head and back (both restored); a reset to a commit
# with no root of its own (the nearest root restored, the rows between
# written) and back; a retention pass; the freezing pass; a page sweep;
# VACUUM of a table every root shares.  And the roots' slot pages, each.
sub measure
{
	my ($node) = @_;
	my %m;

	$m{commit} = counted($node, q{UPDATE app.t1 SET v = v + 1 WHERE id = 1;
		SELECT dolt_commit('-am', 'measured') IS NOT NULL});
	my $head = sql($node, q{SELECT dolt_hashof('HEAD')});
	my $prev = sql($node, q{SELECT dolt_hashof('HEAD~1')});
	$m{reset_back} = counted($node, qq{SELECT dolt_reset('--hard', '$prev')});
	$m{reset_forth} = counted($node, qq{SELECT dolt_reset('--hard', '$head')});
	my $rootless = sql($node, q{SET debug_prolly_page_roots = off;
		UPDATE app.t1 SET v = v + 1 WHERE id = 2;
		SELECT (dolt_commit('-am', 'no root'))[1]});
	sql($node, q{UPDATE app.t1 SET v = v + 1 WHERE id = 3;
		SELECT dolt_commit('-am', 'after') IS NOT NULL});
	my $after = sql($node, q{SELECT dolt_hashof('HEAD')});
	$m{reset_nearest} = counted($node, qq{SELECT dolt_reset('--hard', '$rootless')});
	sql($node, qq{SELECT dolt_reset('--hard', '$after')});
	$m{retention} = counted($node, q{SELECT test_retain_roots('main')});
	$m{freeze} = counted($node, q{SELECT test_freeze_kept()});
	$m{sweep} = counted($node, q{SELECT count(*) FROM prolly_page_sweep()});
	$m{vacuum} = counted($node, q{VACUUM app.t1});

	my ($held, $slots) = split /\|/, sql($node, q{SELECT held, slots FROM test_bmap_roots()});
	$m{storage} = { slots_per_root => $held > 0 ? $slots / $held : 0 };
	return \%m;
}

# Is operation $op's counter $c flat between $small and $big (no more than
# twice, plus a few for the trees' depth, at the bigger size)?  Under TODO
# with the issue that tracks it, if $todo says it isn't yet.
sub flat
{
	my ($small, $big, $op, $c, $label, $todo) = @_;
	my ($s, $b) = ($small->{$op}{$c} // 0, $big->{$op}{$c} // 0);
	my $ok;
	{
		local $TODO = $todo;
		$ok = cmp_ok($b, '<=', 2 * $s + 8, "$op: $c flat ($label: $s, then $b)");
	}
	return $ok;
}

# Every operation's counters reported, whatever they show.
sub report
{
	my ($label, $m) = @_;
	for my $op (sort keys %$m)
	{
		note("$label $op: "
			  . join(', ', map { "$_=$m->{$op}{$_}" } sort keys %{ $m->{$op} }));
	}
	return;
}

# The checks the scale tests make, between two sizes' measures: each
# operation's work flat, but for those still O(roots), each under TODO with
# its issue.
our %KNOWN = (
	'sweep:bmap_roots_pages_read' => 'sweeps count every root (#1187)',
);

sub check_flat
{
	my ($small, $big, $label) = @_;
	my @checks = (
		[ commit => 'refs_nodes_written' ],
		[ commit => 'kept_entries_read' ],
		[ commit => 'kept_entries_written' ],
		[ commit => 'bmap_roots_pages_read' ],
		[ reset_back => 'kept_entries_read' ],
		[ reset_back => 'kept_entries_written' ],
		[ reset_back => 'bmap_roots_pages_read' ],
		[ reset_forth => 'kept_entries_read' ],
		[ reset_forth => 'bmap_roots_pages_read' ],
		[ reset_nearest => 'kept_entries_read' ],
		[ reset_nearest => 'bmap_roots_pages_read' ],
		[ retention => 'kept_entries_read' ],
		[ retention => 'kept_entries_written' ],
		[ retention => 'refs_nodes_written' ],
		[ freeze => 'kept_entries_read' ],
		[ sweep => 'bmap_roots_pages_read' ],
		[ vacuum => 'bmap_roots_pages_read' ],
		[ vacuum => 'bmap_refcount_reads' ],
		[ storage => 'slots_per_root' ],);
	for my $k (@checks)
	{
		my ($op, $c) = @$k;
		flat($small, $big, $op, $c, $label, $KNOWN{"$op:$c"});
	}
	return;
}

1;
