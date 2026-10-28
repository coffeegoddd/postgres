# Copyright (c) 2026, PostgreSQL Global Development Group

# A retire frees a severed tree: once commits are severed (delete mode)
# and the retires the store then chooses have run, every chunk that only
# severed history reached is gone.  What only severed history reaches is
# what the severed commits' trees reached (walked as each commit is made)
# that a full collection, run on the same state instead, frees.  Checked
# two ways, each against a full collection of a copy of the database
# (CREATE DATABASE ... TEMPLATE), chunk for chunk:
# - each retire keeps, of the generations it retires, exactly what the
#   full collection keeps of them.  Every file with garbage is rewritten
#   (prolly.gc_rewrite_below at its largest), so neither keeps a file whole
#   and the two must agree exactly;
# - once the store chooses no more retires, no file holds a chunk only
#   severed history reached unless less than half the file is garbage
#   (one a retire may keep whole, its garbage below the share that has a
#   retire choose it).  A file kept whole while its chunks were live is
#   chosen again once the severs make them garbage.
# Three orders:
#   (a) collect, sever, retire, cycle after cycle;
#   (b) sever, a default collection, then the retires, cycle after cycle;
#   (c) several severs, each followed by its retires, in a row with no
#       commit between them.
# After every retire, and at the end of each sever's retires, the store's
# files on disk agree with its own accounting: each chunk-store file in
# the database's directory is one the manifest names, each one it names
# is there with the size it records, the files a retire drops are gone
# from disk, and the sizes the store reports are the files' real sizes.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('gen_retire_frees_severed');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
prolly.autofold_threshold = 0
prolly.gc_rewrite_below = 2147483647
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

sub sql { my ($db, $q) = @_; return $node->safe_psql($db, $q); }

sub db_dir
{
	my ($db) = @_;
	return $node->data_dir . '/base/'
	  . sql('postgres', qq{SELECT oid FROM pg_database WHERE datname = '$db'});
}

# The store's files on disk: name => size.
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

# The table files and segments the manifest names: name => size.
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

# The store's accounting and the filesystem agree: every table file and
# segment on disk is named, every one named is on disk at the size the
# manifest has, and prolly_store_sizes() is the sum of the real sizes.
sub check_disk
{
	my ($db, $label) = @_;
	my $disk = disk_files($db);
	my $named = named_files($db);
	my @orphans = sort grep { $_ =~ $NAMED_RE && !exists $named->{$_} } keys %$disk;
	my @missing = sort grep { !exists $disk->{$_} } keys %$named;
	my @wrong = sort grep { exists $disk->{$_} && $disk->{$_} != $named->{$_} }
	  keys %$named;
	is(join(',', @orphans), '', "$label: no file on disk the manifest doesn't name");
	is(join(',', @missing), '', "$label: every file the manifest names is on disk");
	is(join(',', map { "$_ disk $disk->{$_} named $named->{$_}" } @wrong), '',
		"$label: each named file's size is its size on disk");
	my ($tb, $jb) = split /\|/, sql($db,
		q{SELECT table_file_bytes, journal_bytes FROM prolly_store_sizes()});
	my ($dt, $dj) = (0, 0);
	for (keys %$disk)
	{
		$dt += $disk->{$_} if /^prolly_table_/;
		$dj += $disk->{$_} if /^prolly_journal/;
	}
	is("$tb $jb", "$dt $dj",
		"$label: the store's sizes are its table files' and segments' on disk");
}

# The hashes the settled files of generations lo to hi hold.
sub gen_hashes
{
	my ($db, $lo, $hi) = @_;
	my %h = map { $_ => 1 } split /\n/, sql($db, qq{
		SELECT DISTINCT h FROM test_chunk_generations() g,
			test_chunk_file_hashes(g.name) h
		WHERE g.settled AND g.epoch_lo >= $lo AND g.epoch_hi <= $hi});
	return \%h;
}

sub all_hashes
{
	my ($db) = @_;
	my %h = map { $_ => 1 } split /\n/, sql($db, q{
		SELECT DISTINCT h FROM test_chunk_table_files() f,
			test_chunk_file_hashes(f.name) h});
	return \%h;
}

sub gen_files
{
	my ($db, $lo, $hi) = @_;
	return [ split /\n/, sql($db, qq{
		SELECT name FROM test_chunk_generations()
		WHERE settled AND epoch_lo >= $lo AND epoch_hi <= $hi ORDER BY name}) ];
}

# Retire generations lo to hi of db, checked against a full collection of a
# copy made just before: the retire keeps of those generations exactly
# what the full collection keeps of them.
my $copies = 0;
sub retire_checked
{
	my ($db, $lo, $hi, $label) = @_;
	my $copy = 'oracle_' . ++$copies;
	my $before = gen_hashes($db, $lo, $hi);
	my $files_before = gen_files($db, $lo, $hi);
	sql('postgres', qq{CREATE DATABASE $copy TEMPLATE $db});
	sql($copy, q{SELECT * FROM test_chunk_gc(true)});
	my $live = all_hashes($copy);
	sql('postgres', qq{DROP DATABASE $copy});

	# (where the store's garbage is as the retire begins, for a failure's
	# report)
	my @where;
	for (split /\n/, sql($db, q{SELECT name || ' ' || epoch_lo || ' ' || epoch_hi
		|| ' ' || garbage FROM test_chunk_generations() WHERE settled ORDER BY epoch_lo}))
	{
		my ($name, $glo, $ghi, $est) = split / /;
		my @h = split /\n/, sql($db, qq{SELECT test_chunk_file_hashes('$name')});
		my $dead = grep { !$live->{$_} } @h;
		push @where, "$glo-$ghi: $dead of " . scalar(@h) . " garbage (estimate $est)";
	}

	my ($kept, $freed) = split /\|/,
	  sql($db, qq{SELECT kept, freed FROM test_chunk_retire($lo, $hi)});
	my $after = gen_hashes($db, $lo, $hi);
	my $want = grep { $live->{$_} } keys %$before;
	my @dead_kept = grep { !$live->{$_} } keys %$after;
	is(scalar(keys %$after), $want,
		"$label: the retire of generations $lo-$hi keeps what a full collection "
		  . "keeps of their " . scalar(keys %$before) . " chunks (retire kept $kept, freed $freed)");
	is(scalar(@dead_kept), 0,
		"$label: no chunk of generations $lo-$hi that a full collection frees is left")
	  or diag("the store's settled files as the retire began: " . join('; ', @where));

	# the files it dropped are gone from disk, the rest agrees
	my $now = named_files($db);
	my $disk = disk_files($db);
	my @dropped = grep { !exists $now->{$_} } @$files_before;
	my @left = grep { exists $disk->{$_} } @dropped;
	is(join(',', @left), '', "$label: the files the retire dropped are gone from disk");
	check_disk($db, $label);
}

sub setup
{
	my ($db) = @_;
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
	note_closure($db, 0);
	sql($db, q{SELECT * FROM test_chunk_gc(false)});
}

# A twentieth of the rows, spread over every leaf, committed as of day i.
sub churn
{
	my ($db, $i) = @_;
	sql($db, qq{UPDATE v.t SET rev = $i WHERE id % 20 = $i % 20});
	sql($db, qq{SELECT dolt_commit('-am', 'c$i', '--date', day($i)::text)});
	note_closure($db, $i);
}

# The generation the last collection settled.
sub generation { return sql($_[0], q{SELECT test_chunk_generation()}); }

# What each commit's trees reach (its root's and dependencies', those
# stored), walked as it is made, in a copy whose refs' root is moved to
# each: by database, then commit day.
my %closure;
sub note_closure
{
	my ($db, $day) = @_;
	my @roots = split /\n/, sql($db, q{
		SELECT h FROM test_commit_roots('main') r, LATERAL (VALUES (r.root),
			(r.dependencies)) v(h) WHERE h IS NOT NULL AND test_chunk_has(h)});
	my %reached;
	my $copy = 'walk_' . ++$copies;
	sql('postgres', qq{CREATE DATABASE $copy TEMPLATE $db});
	for my $r (@roots)
	{
		sql($copy, qq{SELECT test_chunk_move_root(test_chunk_root(), '$r')});
		$reached{$_} = 1 for split /\n/,
		  sql($copy, q{SELECT DISTINCT h FROM test_chunk_reachable() h});
	}
	sql('postgres', qq{DROP DATABASE $copy});
	$closure{$db}{$day} = \%reached;
}

# What the trees of the commits dated before day d reached.
sub severed_closure
{
	my ($db, $d) = @_;
	my %reached;
	for my $day (grep { $_ < $d } keys %{ $closure{$db} })
	{
		$reached{$_} = 1 for keys %{ $closure{$db}{$day} };
	}
	return \%reached;
}

# Sever the commits dated before day d, then (with gc_between, after a
# default collection) run the retires the store chooses, until it chooses
# none, each checked against a full collection of a copy.  Then check
# what is left: what only the severed commits reached (what their trees
# reached as they were made, that a full collection of a copy now frees) is gone from every file but one whose
# garbage is less than half of it, which a retire may keep whole.
sub sever_and_retire
{
	my ($db, $d, $label, $gc_between) = @_;
	my $reached = severed_closure($db, $d);
	sql($db, qq{SELECT dolt_history_drop(day($d))});
	sql($db, q{SELECT * FROM test_chunk_gc(false)}) if $gc_between;
	my $passes = 0;
	while (1)
	{
		my $c = sql($db, q{SELECT lo || ' ' || hi FROM test_chunk_retire_choose()
			WHERE lo IS NOT NULL});
		last if $c eq '' || ++$passes > 8;
		my ($lo, $hi) = split / /, $c;
		retire_checked($db, $lo, $hi, "$label, retire $passes");
	}
	ok($passes <= 8, "$label: the store stops choosing retires");

	my $copy = 'oracle_' . ++$copies;
	sql('postgres', qq{CREATE DATABASE $copy TEMPLATE $db});
	sql($copy, q{SELECT * FROM test_chunk_gc(true)});
	my $live = all_hashes($copy);
	sql('postgres', qq{DROP DATABASE $copy});
	my @left;
	my $severed_left = 0;
	for (split /\n/, sql($db, q{SELECT name || ' ' || epoch_lo || ' ' || epoch_hi
		FROM test_chunk_generations() WHERE settled ORDER BY epoch_lo}))
	{
		my ($name, $lo, $hi) = split / /;
		my @h = split /\n/, sql($db, qq{SELECT test_chunk_file_hashes('$name')});
		my $dead = grep { !$live->{$_} } @h;
		my $severed = grep { !$live->{$_} && $reached->{$_} } @h;
		next if $severed == 0 || 2 * $dead < scalar(@h);
		$severed_left += $severed;
		push @left, "generations $lo-$hi: $severed of " . scalar(@h)
		  . " chunks only severed history reached, $dead garbage";
	}
	is(join('; ', @left), '',
		"$label: what only severed history reached is gone but from files under half garbage");
	is($severed_left, 0, "$label: chunks only severed history reached left");
	check_disk($db, "$label, at the end");
}

# (a) collect, sever, retire: each cycle keeps the newest three commits,
# severing the one before them.
setup('ra');
for my $i (1 .. 10)
{
	churn('ra', $i);
	sql('ra', q{SELECT * FROM test_chunk_gc(false)});
	sever_and_retire('ra', $i - 2, "(a) cycle $i", 0) if $i >= 3;
}

# (b) as (a), with a default collection between the sever and the
# retires.
setup('rb');
for my $i (1 .. 10)
{
	churn('rb', $i);
	sql('rb', q{SELECT * FROM test_chunk_gc(false)});
	sever_and_retire('rb', $i - 2, "(b) cycle $i", 1) if $i >= 3;
}

# (c) six commits, each settled; then the oldest severed and the retires
# chosen run, one sever after another, five times in a row.
setup('rc');
for my $i (1 .. 5)
{
	churn('rc', $i);
	sql('rc', q{SELECT * FROM test_chunk_gc(false)});
}
sever_and_retire('rc', $_, "(c) sever $_", 0) for (1 .. 5);
is(sql('rc', q{SELECT count(*) FROM v.t WHERE rev = id % 20 OR rev = 0}), '1000',
	'(c) the rows read');
ok(sql('rc', q{SELECT test_chunk_verify_generations()}) > 0,
	'(c) the generations check out');

$node->stop;
done_testing();
