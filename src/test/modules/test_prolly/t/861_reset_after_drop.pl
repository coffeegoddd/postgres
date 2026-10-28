# Copyright (c) 2026, PostgreSQL Global Development Group

# A table dropped on a branch and brought back by a hard reset to the
# head is made again as every relation it was: the table, its indexes,
# its TOAST table and that one's index, and its change log, each by its
# OID.  So the reset gives it the pages another branch keeps as of the
# head, writing none of its rows, and the table's kept roots, which name
# its relations by OID, still serve it: a later hard reset to an older
# commit, at any depth, gives back that commit's pages and writes no row.
# Without that, the reset to the head wrote every row, and each later
# reset wrote the rows between the head and its commit.  With no root of
# the head kept, the reset writes the table's rows (its indexes built
# after them), and the table is right and its indexes check out.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('reset_after_drop');
$node->init;
$node->append_conf('postgresql.conf', q{
wal_level = replica
autovacuum = off
prolly_fold_worker = off
fsync = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# (the last line: a checkout's own output comes first)
sub on
{
	my ($branch, $q) = @_;
	my @out = split /\n/, sql(qq{SELECT dolt_checkout('$branch') IS NOT NULL; $q});
	return $out[-1];
}

sql(q{ALTER DATABASE postgres SET prolly.history_window = '100 years';
	ALTER DATABASE postgres SET prolly.history_min_commits = 50;
	ALTER DATABASE postgres SET prolly.root_interval = 0;
	ALTER DATABASE postgres SET prolly.root_interval_commits = 1;
	CREATE EXTENSION amcheck});

my @names = qw(apply_rows apply_loads move_tables_written moves_root_kept
  move_tables_shared relations_restored);
my $names = join(', ', map { "'$_'" } @names);
sub counters
{
	my %c = split /[|\n]/,
	  sql(qq{SELECT name, value FROM pg_stat_prolly WHERE name IN ($names)});
	$c{$_} //= 0 for @names;
	return \%c;
}

# a statement on branch $branch: what it counted
sub counted
{
	my ($branch, $q) = @_;
	my $before = counters();
	on($branch, qq{$q; SELECT pg_stat_force_next_flush()});
	my $after = counters();
	return { map { $_ => $after->{$_} - $before->{$_} } @names };
}

my $n = 0;
# a table (with a secondary index and a TOASTed column, if $wide) on
# branch fixture, @commits made of it, each changing the same rows; then a
# branch made at its head, its table dropped and brought back by a hard
# reset to the head, then hard resets of it to older commits
sub case
{
	my ($wide, $roots) = @_;
	$n++;
	my $t = "app.t$n";
	my $what = ($wide ? 'with a secondary index and TOAST' : 'plain')
	  . ($roots ? '' : ', no root of the head');
	# (no root kept of the fixture's later commits)
	my $set = $roots ? '' : 'SET prolly.root_interval_commits = 1000;';
	my $digest = qq{SELECT md5(string_agg(id || ':' || rev || ':' || md5(payload), ',' ORDER BY id)) FROM $t};

	sql(qq{SELECT dolt_checkout('main') IS NOT NULL;
		CREATE TABLE $t (id int PRIMARY KEY, rev int NOT NULL, payload text NOT NULL)});
	sql(qq{CREATE INDEX t${n}_rev ON $t (rev)}) if $wide;
	sql(qq{INSERT INTO $t SELECT g, 0, }
		  . ($wide
			? q{(SELECT string_agg(md5(g::text || i::text), '') FROM generate_series(1, 300) i)}
			: q{md5(g::text)})
		  . q{ FROM generate_series(1, 5000) g;
		SELECT dolt_commit('-Am', 'load') IS NOT NULL;}
		  . qq{SELECT dolt_branch('fixture$n') IS NOT NULL});
	my %at;
	my @commits;
	for my $r (1 .. 12)
	{
		my $h = on("fixture$n", qq{$set UPDATE $t SET rev = $r WHERE id % 50 = 0;
			SELECT (dolt_commit('-am', 'r$r'))[1]});
		push @commits, $h;
		$at{$h} = on("fixture$n", $digest);
	}
	my $head = $commits[-1];
	sql(qq{SELECT dolt_branch('scratch$n', '$head') IS NOT NULL});
	is(on("scratch$n", $digest), $at{$head}, "$what: the scratch branch at the head");
	my $toast = on("scratch$n", qq{SELECT reltoastrelid <> 0 AND
		(SELECT count(*) FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class
			WHERE oid = '$t'::regclass)) = 1 FROM pg_class WHERE oid = '$t'::regclass});
	is($toast, 't', "$what: the table has a TOAST table");
	my $rels = qq{SELECT string_agg(c.oid::text, ',' ORDER BY c.oid) FROM pg_class c
		WHERE c.oid = '$t'::regclass
		   OR c.oid IN (SELECT indexrelid FROM pg_index WHERE indrelid = '$t'::regclass)
		   OR c.oid = (SELECT reltoastrelid FROM pg_class WHERE oid = '$t'::regclass)
		   OR c.relname ~ ('^pg_(toast|prolly_log)_' || '$t'::regclass::oid)};
	my $before = on("scratch$n", $rels);

	my $indexes = qq{SELECT count(*) FROM (SELECT bt_index_check(indexrelid, true)
		FROM pg_index WHERE indrelid IN ('$t'::regclass,
			(SELECT reltoastrelid FROM pg_class WHERE oid = '$t'::regclass))) s};

	on("scratch$n", qq{DROP TABLE $t});
	my $d = counted("scratch$n", qq{SELECT dolt_reset('--hard', '$head')});
	if (!$roots)
	{
		is($d->{apply_rows}, 5000, "$what: the reset bringing the table back writes its rows");
		is(on("scratch$n", $digest), $at{$head}, "$what: the table holds the head's rows");
		is(on("scratch$n", $indexes), $wide ? 3 : 2, "$what: its indexes check out");
		is(on("scratch$n", q{SELECT count(*) FROM dolt_status}), '0',
			"$what: the working set is clean");
		return;
	}
	is($d->{apply_rows}, 0, "$what: the reset bringing the table back writes no row");
	is($d->{apply_loads}, 0, "$what: nor loads the table");
	is($d->{move_tables_written}, 0, "$what: nor writes any table");
	ok($d->{move_tables_shared} > 0, "$what: it shares the head's kept pages");
	is(on("scratch$n", $digest), $at{$head}, "$what: the table holds the head's rows");
	is(on("scratch$n", $rels), $before, "$what: made again as every relation it was");

	for my $depth (1, 5, 11)
	{
		my $to = $commits[-1 - $depth];
		$d = counted("scratch$n", qq{SELECT dolt_reset('--hard', '$to')});
		is($d->{moves_root_kept}, 1, "$what, depth $depth: the commit's root found");
		is($d->{apply_rows}, 0, "$what, depth $depth: no row written");
		is($d->{move_tables_written}, 0, "$what, depth $depth: no table written");
		is(on("scratch$n", $digest), $at{$to}, "$what, depth $depth: the commit's rows");
		is(on("scratch$n", $indexes), $wide ? 3 : 2,
			"$what, depth $depth: its indexes check out");
		is(on("scratch$n", q{SELECT count(*) FROM dolt_status}), '0',
			"$what, depth $depth: the working set is clean");
	}
}

sql(q{CREATE SCHEMA app VERSIONED});
case(0, 1);
case(1, 1);
case(1, 0);

$node->stop;
done_testing();
