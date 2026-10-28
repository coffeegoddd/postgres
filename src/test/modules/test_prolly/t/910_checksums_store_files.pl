# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_checksums on a cluster with chunk stores: the store's files (its
# journal's segments and index, its table files, its manifest, the
# window's seal) sit in the database directories but aren't relation
# files, and hold no pages; --check passes over them, and --enable leaves
# them as they were.  A base backup's checksum verification passes over
# them too.

use strict;
use warnings FATAL => 'all';
use File::Compare;
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# the chunk store's files in a cluster's database directories
sub store_files
{
	my ($node) = @_;
	my @files;
	my $base = $node->data_dir . '/base';
	opendir(my $dh, $base) or die "could not open $base: $!";
	foreach my $db (grep { /^\d+$/ } readdir($dh))
	{
		opendir(my $dbh, "$base/$db") or die;
		push @files, map { "$base/$db/$_" } grep { /^prolly_/ } readdir($dbh);
		closedir($dbh);
	}
	closedir($dh);
	return sort @files;
}

# A cluster with checksums, a versioned schema and commits, collected.
my $node = PostgreSQL::Test::Cluster->new('checksums_store');
$node->init(allows_streaming => 1, extra => ['--data-checksums']);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA cs VERSIONED;
	CREATE TABLE cs.t (id int PRIMARY KEY, v text);
	INSERT INTO cs.t SELECT g, repeat('v', 100) || g FROM generate_series(1, 5000) g;
	SELECT dolt_commit('-Am', 'one');
	UPDATE cs.t SET v = v || '!' WHERE id % 7 = 0;
	SELECT dolt_commit('-Am', 'two');
	SELECT prolly_gc(false);
	SELECT dolt_commit('--allow-empty', '-m', 'three')});

# (a base backup verifies checksums of the files named as relations only)
command_checks_all(
	[ 'pg_basebackup', '--pgdata' => PostgreSQL::Test::Utils::tempdir . '/bb',
	  '--checkpoint' => 'fast', '--host' => $node->host, '--port' => $node->port ],
	0, [qr/^$/], [qr/^$/], 'pg_basebackup verifies checksums without complaint');
$node->stop;

my @files = store_files($node);
my %kinds;
foreach (@files)
{
	$kinds{index}++ if m{/prolly_index$};
	$kinds{journal}++ if m{/prolly_journal};
	$kinds{table}++ if m{/prolly_table_};
	$kinds{manifest}++ if m{/prolly_manifest$};
}
is(join(',', map { $kinds{$_} ? $_ : "no $_" } qw(index journal table manifest)),
	'index,journal,table,manifest', 'the store\'s files are in the database directories');

command_ok([ 'pg_checksums', '--check', '--pgdata' => $node->data_dir ],
	'pg_checksums --check passes over the chunk store\'s files');

# --enable on it is still refused, for the block-mapped page store
command_ok([ 'pg_checksums', '--disable', '--pgdata' => $node->data_dir ],
	'pg_checksums --disable');
command_fails_like(
	[ 'pg_checksums', '--enable', '--pgdata' => $node->data_dir ],
	qr/block-mapped storage/,
	'pg_checksums --enable refuses block-mapped storage, not the store\'s files');

# A cluster without checksums whose store has chunks and a table file but
# no versioned table: --enable runs, and leaves the store's files as they
# were.
my $plain = PostgreSQL::Test::Cluster->new('checksums_store_plain');
$plain->init(extra => ['--no-data-checksums']);
$plain->append_conf('postgresql.conf', 'autovacuum = off');
$plain->start;
my $out = $plain->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	SELECT test_chunk_put(convert_to(repeat('chunk ', 3000), 'UTF8')) AS a \gset
	SELECT test_chunk_move_root(NULL, test_chunk_put_node(1, ARRAY[:'a']));
	SELECT test_chunk_gc(false);
	SELECT :'a'});
my $hash = (split /\n/, $out)[-1];
$plain->stop;
my @plain_files = store_files($plain);
ok(scalar(grep { m{/prolly_table_} } @plain_files) > 0, 'the store has a table file');
my $copies = PostgreSQL::Test::Utils::tempdir;
foreach my $f (@plain_files)
{
	(my $name = $f) =~ s{.*/base/(\d+)/}{$1_};
	copy($f, "$copies/$name") or die "could not copy $f: $!";
}
command_ok([ 'pg_checksums', '--enable', '--pgdata' => $plain->data_dir ],
	'pg_checksums --enable');
my @changed;
foreach my $f (@plain_files)
{
	(my $name = $f) =~ s{.*/base/(\d+)/}{$1_};
	push @changed, $f if compare($f, "$copies/$name") != 0;
}
is(join(' ', @changed), '', 'the store\'s files are as they were');
command_ok([ 'pg_checksums', '--check', '--pgdata' => $plain->data_dir ],
	'pg_checksums --check after --enable');
$plain->start;
is($plain->safe_psql('postgres', qq{SELECT test_chunk_has('$hash')}), 't',
	'the store reads its chunk');
$plain->stop;

done_testing();
