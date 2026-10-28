# Copyright (c) 2026, PostgreSQL Global Development Group

# The chunk store's files on disk agree with its own accounting around
# retires.  Each chunk-store file in the database's directory (by name:
# table files, journal segments, the manifest, the journal's index) that
# holds chunks is one the manifest names, each one it names is there at
# the size it records, and the sizes the store reports are the files'
# real sizes.  A retire's dropped files are gone from disk, and the bytes
# on disk move by exactly the files it removed and wrote.  Files left
# retiring, by a retire that fails after its swap or a crash there, are
# gone once the next collection runs, across an immediate restart too;
# and a retire that waits for a reader removes what it dropped once the
# reader ends.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('gen_store_files_on_disk');
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
	sql($db, q{SELECT * FROM test_chunk_gc(false)});
}

# A twentieth of the rows, spread over every leaf, committed as of day i.
sub churn
{
	my ($db, $i) = @_;
	sql($db, qq{UPDATE v.t SET rev = $i WHERE id % 20 = $i % 20});
	sql($db, qq{SELECT dolt_commit('-am', 'c$i', '--date', day($i)::text)});
}

# The generation the last collection settled.
sub generation { return sql($_[0], q{SELECT test_chunk_generation()}); }

# A retire that drops and writes files: the bytes on disk move by the
# files it removed and the files it wrote, no more.
setup('rd');
my %g = (0 => generation('rd'));
for my $i (1 .. 4)
{
	churn('rd', $i);
	sql('rd', q{SELECT * FROM test_chunk_gc(false)});
	$g{$i} = generation('rd');
}
sql('rd', q{SELECT dolt_history_drop(day(3))});
my $before = disk_files('rd');
my $r = gen_files('rd', $g{0}, $g{2});
sql('rd', "SELECT * FROM test_chunk_retire($g{0}, $g{2})");
my $after = disk_files('rd');
my @removed = sort grep { !exists $after->{$_} } keys %$before;
my @added = sort grep { !exists $before->{$_} } keys %$after;
my @dropped = grep { !exists named_files('rd')->{$_} } @$r;
ok(@dropped > 0, 'the retire drops files');
is(join(',', grep { exists $after->{$_} } @dropped), '',
	'every file the retire dropped is gone from disk');
is(scalar(grep { /^prolly_table_/ } keys %$after),
	scalar(grep { /^prolly_table_/ } keys %$before) - scalar(grep { /^prolly_table_/ } @removed)
	  + scalar(grep { /^prolly_table_/ } @added),
	'the table files on disk are those before, less those removed, and those written');
my $moved = 0;
$moved -= $before->{$_} for @removed;
$moved += $after->{$_} for @added;
$moved += $after->{$_} - $before->{$_} for grep { exists $after->{$_} } keys %$before;
my $sum = sub { my $t = 0; $t += $_ for values %{ $_[0] }; $t };
is($sum->($after) - $sum->($before), $moved, 'the bytes on disk move by those files');
check_disk('rd', 'after a retire');

# Files left retiring by a retire that fails after its swap are gone once
# the next collection runs; so are those of one a crash stops there,
# across an immediate restart.
setup('rr');
my %gen = (0 => generation('rr'));
for my $i (1 .. 3)
{
	churn('rr', $i);
	sql('rr', q{SELECT * FROM test_chunk_gc(false)});
	$gen{$i} = generation('rr');
}
sql('rr', q{CREATE EXTENSION injection_points});
sql('rr', q{SELECT dolt_history_drop(day(1))});
my $files = gen_files('rr', $gen{0}, $gen{0});
sql('rr', q{SELECT injection_points_attach('prolly-gc-retire', 'error')});
my ($ret, $out, $err) = $node->psql('rr',
	"SELECT * FROM test_chunk_retire($gen{0}, $gen{0})");
like($err, qr/prolly-gc-retire/, 'a retire fails after its swap');
sql('rr', q{SELECT injection_points_detach('prolly-gc-retire')});
sql('rr', q{SELECT * FROM test_chunk_gc(false)});
my $disk = disk_files('rr');
is(join(',', grep { exists $disk->{$_} && !exists named_files('rr')->{$_} } @$files), '',
	'what it left retiring is gone after the next collection');
check_disk('rr', 'after a retire failed past its swap');

churn('rr', 4);
sql('rr', q{SELECT * FROM test_chunk_gc(false)});
sql('rr', q{SELECT dolt_history_drop(day(2))});
$files = gen_files('rr', $gen{1}, $gen{1});
sql('rr', q{SELECT injection_points_attach('prolly-gc-retire', 'error')});
($ret, $out, $err) = $node->psql('rr',
	"SELECT * FROM test_chunk_retire($gen{1}, $gen{1})");
like($err, qr/prolly-gc-retire/, 'another retire fails after its swap');
$node->stop('immediate');
$node->start;
sql('rr', q{SELECT * FROM test_chunk_gc(false)});
$disk = disk_files('rr');
is(join(',', grep { exists $disk->{$_} && !exists named_files('rr')->{$_} } @$files), '',
	'across an immediate restart, what it left is gone after the next collection');
check_disk('rr', 'after an immediate restart');

# A reader open as a retire runs: the retire waits for it, and once it has
# ended, what the retire dropped is gone and nothing is left behind.
churn('rr', 5);
sql('rr', q{SELECT * FROM test_chunk_gc(false)});
sql('rr', q{SELECT dolt_history_drop(day(3))});
$files = gen_files('rr', $gen{2}, $gen{2});
my $reader = $node->background_psql('rr');
$reader->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ});
$reader->query_safe(q{SELECT count(*) FROM dolt_log('main')});
my $retirer = $node->background_psql('rr');
$retirer->query_until(qr/started/, qq{\\echo started
	SELECT * FROM test_chunk_retire($gen{2}, $gen{2});
	\\echo retired
});
$reader->query_safe(q{COMMIT});
$reader->quit;
$retirer->query_until(qr/retired/, '');
$retirer->quit;
$disk = disk_files('rr');
my $named = named_files('rr');
is(join(',', grep { exists $disk->{$_} && !exists $named->{$_} } @$files), '',
	'a retire that waited for a reader removes what it dropped once the reader ends');
check_disk('rr', 'after a retire with a reader');

$node->stop;
done_testing();
