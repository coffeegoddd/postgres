# Copyright (c) 2026, PostgreSQL Global Development Group

# Steady state: with the live data held constant, cycles of churn, commit,
# default collection, sever (delete mode, keeping the newest three
# commits) and the retires the store chooses leave a store that doesn't
# grow.  Measured on the filesystem (the chunk-store files in the
# database's directory), and in the store's own accounting, which must
# agree with it:
# - Disk: over the last ten cycles the store's bytes on disk don't grow
#   by more than the share of live data an old file may hold as garbage
#   before it is chosen (t = 1/2 of it, so half the live data), plus one
#   cycle's writes.
# - Residual garbage: at the end the store is at most what a full
#   collection keeps divided by (1 - t), twice it, plus the floating
#   garbage of one window (the bytes the window's three cycles settled).
#   A full collection then frees no more than that bound allows.
# - No file pinned: every settled file older than the window holds less
#   than t garbage; one kept whole once is chosen again as more of it
#   becomes garbage.
# - Every collection and retire chosen succeeds, and the generations check out after
#   each cycle; the choices end, and a generation is chosen again, to free
#   nothing again, only on new evidence.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('gen_steady_state');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
prolly.autofold_threshold = 0
});
$node->start;

# The chunk store's own files, by name: everything else in the database's
# directory (relation files, notes other components keep) is left out.
my @STORE_FILE_PATTERNS = (
	qr/^prolly_table_[0-9a-f]{16}$/,      # table files
	qr/^prolly_journal(_[0-9a-f]{16})?$/,  # journal segments
	qr/^prolly_manifest$/,                 # the manifest
	qr/^prolly_index$/,                    # the journal's index
);
my $NAMED_RE = qr/^prolly_(table_[0-9a-f]{16}|journal(_[0-9a-f]{16})?)$/;

my $T = 0.5;          # the garbage share past which a file is retired
my $WINDOW = 3;       # commits kept
my $CYCLES = 24;
my $LAST = 10;

sub sql { my ($db, $q) = @_; return $node->safe_psql($db, $q); }

sub db_dir
{
	my ($db) = @_;
	return $node->data_dir . '/base/'
	  . sql('postgres', qq{SELECT oid FROM pg_database WHERE datname = '$db'});
}

sub disk_files
{
	my ($db) = @_;
	my $dir = db_dir($db);
	my %f;
	opendir(my $d, $dir) or die "opendir $dir: $!";
	for my $name (readdir($d))
	{
		next unless grep { $name =~ $_ } @STORE_FILE_PATTERNS;
		$f{$name} = -s "$dir/$name";
	}
	closedir($d);
	return \%f;
}

sub disk_bytes
{
	my $f = disk_files($_[0]);
	my $b = 0;
	$b += $_ for values %$f;
	return $b;
}

sub named_files
{
	my ($db) = @_;
	my %f;
	for (split /\n/, sql($db, q{
		SELECT name || ' ' || bytes FROM test_chunk_table_files()
		UNION ALL
		SELECT name || ' ' || bytes FROM test_chunk_segments()}))
	{
		my ($n, $b) = split / /;
		$f{$n} = $b;
	}
	return \%f;
}

# The store's accounting agrees with the filesystem: true, or the
# differences.
sub disk_mismatch
{
	my ($db) = @_;
	my $disk = disk_files($db);
	my $named = named_files($db);
	my @bad;
	push @bad, "on disk, not named: $_"
	  for sort grep { $_ =~ $NAMED_RE && !exists $named->{$_} } keys %$disk;
	push @bad, "named, not on disk: $_" for sort grep { !exists $disk->{$_} } keys %$named;
	push @bad, "$_ named at $named->{$_} bytes, $disk->{$_} on disk"
	  for sort grep { exists $disk->{$_} && $disk->{$_} != $named->{$_} } keys %$named;
	my ($tb, $jb) = split /\|/, sql($db,
		q{SELECT table_file_bytes, journal_bytes FROM prolly_store_sizes()});
	my ($dt, $dj) = (0, 0);
	for (keys %$disk)
	{
		$dt += $disk->{$_} if /^prolly_table_/;
		$dj += $disk->{$_} if /^prolly_journal/;
	}
	push @bad, "store sizes $tb+$jb, on disk $dt+$dj" if $tb != $dt || $jb != $dj;
	return join('; ', @bad);
}

my $copies = 0;

# A full collection of a copy of db, every file with garbage rewritten:
# its chunks (a set), its bytes on disk before and after.
sub full_oracle
{
	my ($db) = @_;
	my $copy = 'oracle_' . ++$copies;
	sql('postgres', qq{CREATE DATABASE $copy TEMPLATE $db});
	my $before = disk_bytes($copy);
	sql($copy, q{SET prolly.gc_rewrite_below = 2147483647;
		SELECT * FROM test_chunk_gc(true)});
	my $after = disk_bytes($copy);
	my %live = map { $_ => 1 } split /\n/, sql($copy, q{
		SELECT DISTINCT h FROM test_chunk_table_files() f,
			test_chunk_file_hashes(f.name) h});
	sql('postgres', qq{DROP DATABASE $copy});
	return (\%live, $before, $after);
}

# One workload: the rows whose id is i modulo mod updated in cycle i.
sub steady
{
	my ($db, $mod) = @_;
	sql('postgres', qq{CREATE DATABASE $db});
	sql($db, q{CREATE EXTENSION test_prolly;
		CREATE FUNCTION day(d int) RETURNS timestamptz LANGUAGE sql AS
			$$ SELECT timestamptz '2020-01-01 00:00+00' + make_interval(days => d) $$;
		CREATE FUNCTION pad(id int) RETURNS text LANGUAGE sql IMMUTABLE AS
			$$ SELECT string_agg(md5(k || ':' || id), '') FROM generate_series(1, 30) k $$;
		CREATE SCHEMA v VERSIONED;
		CREATE TABLE v.t (id int PRIMARY KEY, rev int NOT NULL, pad text NOT NULL);
		INSERT INTO v.t SELECT g, 0, pad(g) FROM generate_series(1, 1000) g;
		SELECT dolt_commit('-Am', 'c0', '--date', day(0)::text)});
	sql($db, q{SELECT * FROM test_chunk_gc(false)});

	my (@bytes, @nfiles, @accounted, @young, @gen);
	my (@mismatch, @failed, @endless, @idle, @unverified, %chosen_idle);
	for my $i (1 .. $CYCLES)
	{
		# a share of the rows, spread over every leaf
		sql($db, qq{UPDATE v.t SET rev = $i WHERE id % $mod = $i % $mod});
		sql($db, qq{SELECT dolt_commit('-am', 'c$i', '--date', day($i)::text)});
		my ($gret, $gout, $gerr) = $node->psql($db, q{SELECT * FROM test_chunk_gc(false)});
		push @failed, "cycle $i, default collection: $gerr" if $gret != 0;
		$gen[$i] = sql($db, q{SELECT test_chunk_generation()});
		# (what this cycle settled: its share of the window's floating garbage)
		$young[$i] = sql($db, qq{SELECT coalesce(sum(f.bytes), 0)
			FROM test_chunk_generations() g JOIN test_chunk_table_files() f USING (name)
			WHERE g.settled AND g.epoch_lo = $gen[$i]});
		sql($db, sprintf(q{SELECT dolt_history_drop(day(%d))}, $i - $WINDOW + 1))
		  if $i >= $WINDOW;
		my $pass = 0;
		while (1)
		{
			my $c = sql($db, q{SELECT lo || ' ' || hi FROM test_chunk_retire_choose()
				WHERE lo IS NOT NULL});
			last if $c eq '';
			if (++$pass > 8)
			{
				push @endless, "cycle $i: still choosing $c";
				last;
			}
			my ($lo, $hi) = split / /, $c;
			my ($ret, $out, $err) =
			  $node->psql($db, qq{SELECT * FROM test_chunk_retire($lo, $hi)});
			if ($ret != 0)
			{
				push @failed, "cycle $i, retire $lo-$hi: $err";
				last;
			}
			# (a retire that frees nothing is chosen again only on new evidence)
			push @idle, "cycle $i: $lo-$hi" if $out =~ /^\d+\|0\|/ && $chosen_idle{"$lo $hi"}++;
		}
		my ($ret, $out, $err) =
		  $node->psql($db, q{SELECT test_chunk_verify_generations()});
		push @unverified, "cycle $i: $err" if $ret != 0;
		$bytes[$i] = disk_bytes($db);
		$nfiles[$i] = scalar(grep { /^prolly_table_/ } keys %{ disk_files($db) });
		$accounted[$i] = sql($db, q{SELECT journal_bytes + table_file_bytes
			FROM prolly_store_sizes()});
		my $m = disk_mismatch($db);
		push @mismatch, "cycle $i: $m" if $m ne '';
	}
	note("$db: store bytes on disk by cycle: "
		  . join(' ', map { "$_:$bytes[$_]" } 1 .. $CYCLES));
	is(join(' | ', @mismatch), '',
		"$db: after every cycle, the store's accounting is its files on disk");
	is(join(' | ', @failed), '', "$db: every default collection and retire chosen succeeds");
	is(join(' | ', @unverified), '', "$db: the generations check out after every cycle");
	is(join(' | ', @endless), '', "$db: each cycle's retires end: the store stops choosing");
	is(join(' | ', @idle), '',
		"$db: a generation whose retire freed nothing isn't chosen again, and again freed nothing");

	my ($live, $full_before, $full_after) = full_oracle($db);
	my $window_bytes = 0;
	$window_bytes += $young[$_] for ($CYCLES - $WINDOW + 1 .. $CYCLES);
	note("$db: a full collection keeps $full_after bytes on disk; the window settled $window_bytes");

	# Disk: no growth over the last ten cycles past half the live data and
	# one cycle's writes.
	my $first = $CYCLES - $LAST + 1;
	my $grew = $bytes[$CYCLES] - $bytes[$first];
	my $allowed = int($T * $full_after) + $young[$CYCLES];
	cmp_ok($grew, '<=', $allowed,
		"$db: over the last $LAST cycles the store on disk grows by at most half the live data "
		  . "and a cycle's writes ($bytes[$first] to $bytes[$CYCLES])");
	cmp_ok($nfiles[$CYCLES] - $nfiles[$first], '<=', 2,
		"$db: over the last $LAST cycles the table files on disk grow by at most two "
		  . "($nfiles[$first] to $nfiles[$CYCLES])");
	my $acc_grew = $accounted[$CYCLES] - $accounted[$first];
	cmp_ok($acc_grew, '<=', $allowed,
		"$db: and as the store accounts for it ($accounted[$first] to $accounted[$CYCLES])");

	# Residual garbage: at most live / (1 - t), plus one window's floating
	# garbage.
	my $bound = int($full_after / (1 - $T)) + $window_bytes;
	cmp_ok($bytes[$CYCLES], '<=', $bound,
		"$db: at the end the store on disk is within twice what a full collection keeps "
		  . "and one window's writes");
	cmp_ok($full_before - $full_after, '<=', $bound - $full_after,
		"$db: a full collection then frees no more than that bound allows");

	# No file pinned: each settled file older than the window is less than
	# half garbage.
	my @pinned;
	for (split /\n/, sql($db, qq{SELECT name || ' ' || epoch_lo || ' ' || epoch_hi
		|| ' ' || garbage FROM test_chunk_generations()
		WHERE settled AND epoch_hi < $gen[$CYCLES - $WINDOW + 1] ORDER BY epoch_lo}))
	{
		my ($name, $lo, $hi, $est) = split / /;
		my @h = split /\n/, sql($db, qq{SELECT test_chunk_file_hashes('$name')});
		my $dead = grep { !$live->{$_} } @h;
		push @pinned, "generations $lo-$hi: $dead of " . scalar(@h)
		  . " garbage (estimate $est)"
		  if @h && $dead >= $T * scalar(@h);
	}
	is(join('; ', @pinned), '',
		"$db: no settled file older than the window holds half its chunks as garbage");

	is(sql($db, q{SELECT count(*) FROM v.t}), '1000', "$db: the rows read");

}

# A twentieth of the rows a cycle: each cycle rewrites about a fifth of
# the leaves, so old files turn garbage slowly, over several severs.
steady('ss20', 20);
# A quarter: nearly every leaf each cycle, so each old file is almost all
# garbage by the next sever.
steady('ss4', 4);

$node->stop;
done_testing();
