# Copyright (c) 2026, PostgreSQL Global Development Group

# A restore (a hard reset to a kept commit) gives each table it restores
# new storage that carries the old storage's lineage on, so the branch's
# other kept roots of the table stay its own with nothing renamed, and the
# move reads only the kept entries it uses.  What it touches (the kept
# entries it reads and writes, the captured roots' slot pages it reads)
# is the same with about 10 kept roots of the table as with about 1,000,
# with or without a snapshot older than the move; and resets back and
# forth between two kept commits restore every time, writing no row.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('restore_flat_in_roots');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
fsync = off
});
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

my @names = qw(kept_entries_read kept_entries_written bmap_roots_pages_read
  relations_restored apply_rows);
my $names = join(', ', map { "'$_'" } @names);
sub counters
{
	# (a new session: what the earlier ones counted was flushed as they ended)
	my %c = split /[|\n]/,
	  sql(qq{SELECT name, value FROM pg_stat_prolly WHERE name IN ($names)});
	$c{$_} //= 0 for @names;
	return \%c;
}

# a reset to commit $to: what it touched, counter by counter
sub reset_to
{
	my ($to) = @_;
	my $before = counters();
	sql(qq{SELECT dolt_reset('--hard', '$to')});
	my $after = counters();
	return { map { $_ => $after->{$_} - $before->{$_} } @names };
}

sub commits
{
	my ($from, $n) = @_;
	my $script = '';
	$script .= qq{UPDATE app.t SET v = v + 1 WHERE id = } . ($_ % 500 + 1)
	  . qq{;\nSELECT dolt_commit('-am', 'c$_') IS NOT NULL;\n}
	  for ($from .. $from + $n - 1);
	sql($script);
}

sql(q{ALTER DATABASE postgres SET prolly.history_window = '1000 years'});
sql(q{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int NOT NULL);
	CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT g, 0 FROM generate_series(1, 2000) g});
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.t};
my $a = sql(q{SELECT (dolt_commit('-Am', 'a'))[1]});
my $at_a = sql($digest);
sql(q{UPDATE app.t SET v = -1 WHERE id <= 300});
my $b = sql(q{SELECT (dolt_commit('-am', 'b'))[1]});
my $at_b = sql($digest);

sub kept_roots
{
	return sql(q{SELECT count(*) FROM test_kept_commits('main')});
}

# back and forth between a and b, measured: a's reset, then b's, each as
# the reset before it left the table; and the same with a REPEATABLE READ
# snapshot older than the moves, which goes on reading the storage each
# replaces
sub trips
{
	my ($label) = @_;
	my %m;

	reset_to($a);
	reset_to($b);
	$m{to_a} = reset_to($a);
	is(sql($digest), $at_a, "$label: a's rows");
	$m{to_b} = reset_to($b);
	is(sql($digest), $at_b, "$label: b's rows");

	# (its snapshot taken by a read of another table: one of app.t would
	# hold its lock, which a restore doesn't wait out)
	my $rr = $node->background_psql('postgres');
	$rr->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ;
		SELECT count(*) FROM pg_class});
	$m{snap_to_a} = reset_to($a);
	$m{snap_to_b} = reset_to($b);
	is($rr->query_safe($digest), $at_b,
		"$label: the older snapshot reads the rows it saw");
	$rr->query_safe('COMMIT');
	$rr->quit;
	is(sql($digest), $at_b, "$label: b's rows after the snapshot");

	for my $k (sort keys %m)
	{
		cmp_ok($m{$k}{relations_restored}, '>', 0, "$label $k restored");
		is($m{$k}{apply_rows}, 0, "$label $k wrote no row");
	}
	return \%m;
}

sql(q{CREATE EXTENSION test_prolly});
commits(1, 8);
my $few = kept_roots();
cmp_ok($few, '>=', 8, "about 10 kept roots ($few)");
cmp_ok($few, '<=', 12, 'and no more');
my $small = trips('few roots');

# about 1,000: commits on b, which a and b stay kept under
commits(100, 990);
my $many = kept_roots();
cmp_ok($many, '>=', 990, "about 1,000 kept roots ($many)");
my $big = trips('many roots');

for my $k (sort keys %$small)
{
	for my $c (qw(kept_entries_read kept_entries_written bmap_roots_pages_read))
	{
		is($big->{$k}{$c}, $small->{$k}{$c},
			"$k: $c the same with $many roots as with $few"
			  . " ($small->{$k}{$c})");
	}
}

$node->stop;
done_testing();
