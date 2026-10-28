# Copyright (c) 2026, PostgreSQL Global Development Group

# A collection that fails after its copies leaves table files no manifest
# names.  The next writer removes them, WAL-logged, and never gives their
# ids again: a file made again under one of them, from copies of files
# removed since, would be removed by redo of the old one's removal after a
# crash, with nothing left to copy it from.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('unnamed_ids');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off");
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE cp');
$node->safe_psql('cp',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');

sub sql { return $node->safe_psql('cp', $_[0]); }

# the table files on disk, and those the manifest names
my $on_disk = q{SELECT coalesce(string_agg(f, ' ' ORDER BY f), '')
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'cp') db,
		pg_ls_dir(d) f
	WHERE f LIKE 'prolly\_table\_%'};
my $named = q{SELECT coalesce(string_agg(name, ' ' ORDER BY name), '')
	FROM test_chunk_table_files()};

# Chunks of 32 kB of noise, the live ones listed under a root naming them.
sql(q{CREATE TABLE live (hash text PRIMARY KEY, md5 text) USING heap});

sub put
{
	my ($name, $count, $live) = @_;
	my $chunk = "test_noise('$name ' || g, 32768)";

	if (!$live)
	{
		sql(qq{SELECT test_chunk_put($chunk) FROM generate_series(1, $count) g});
		return;
	}
	sql(qq{INSERT INTO live SELECT test_chunk_put($chunk), md5($chunk)
		FROM generate_series(1, $count) g});
	my $refs = sql(q{SELECT array_agg(hash ORDER BY hash) FROM live});
	my $root = sql("SELECT test_chunk_put_node(1, '$refs')");
	sql("SELECT test_chunk_move_root(test_chunk_root(), '$root')");
}

sub drop_live
{
	my ($count) = @_;
	sql(qq{DELETE FROM live WHERE hash IN
		(SELECT hash FROM live ORDER BY hash LIMIT $count)});
	put('none', 0, 1);
}

my $reads = q{SELECT count(*) FILTER (WHERE md5(test_chunk_get(hash)) = md5)
	|| ' of ' || count(*) FROM live};
sub all_read { my $c = sql('SELECT count(*) FROM live'); return "$c of $c"; }

put('first live', 40, 1);
put('first dead', 40, 0);
sql('SELECT test_chunk_gc(true)');
put('failed live', 20, 1);
put('failed dead', 20, 0);
drop_live(10);
sql('CHECKPOINT');

# The collection fails after its copies: the files it wrote are on disk,
# named by no manifest.
my ($ok, $out, $err) = $node->psql('cp', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-copied', 'error');
	SELECT test_chunk_gc(true);});
like($err, qr/prolly-gc-copied/, 'a collection fails after its copies');
my %named = map { $_ => 1 } split(/ /, sql($named));
my @left = grep { !$named{$_} } split(/ /, sql($on_disk));
cmp_ok(scalar(@left), '>', 0, 'it leaves files no manifest names');

# The next collection removes them and writes files of its own: none under
# an id of those it removed.
sql('SELECT test_chunk_gc(true)');
my %now = map { $_ => 1 } split(/ /, sql($on_disk));
my @again = grep { $now{$_} } @left;
is(join(' ', @again), '', "the next collection's files have none of their ids");

# A crash, redo from the checkpoint before both: it replays the removals
# and the copies, and the store reads.
$node->stop('immediate');
$node->start;
is(sql($reads), all_read(), 'crash recovery replays both collections');

$node->stop;
done_testing();
