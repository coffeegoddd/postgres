# Copyright (c) 2026, PostgreSQL Global Development Group

# The journal's shared index is saved by checkpoints (prolly_index in the
# database's directory) and read back by the first backend after a start,
# which then reads only the journal written since, not all of it.  The
# file is a cache: chunks appended after it was saved are found after a
# crash, a damaged file is passed over (and the journal read whole), and
# one naming a segment a collection retired since is read for the
# segments still named only.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('jindex');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE ix');
$node->safe_psql('ix', 'CREATE EXTENSION test_prolly');

sub sql { return $node->safe_psql('ix', $_[0]); }

my $dir = $node->data_dir . '/base/'
  . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'ix'});
my $file = "$dir/prolly_index";

# 2 MB of chunks, the last the root.
my $root = sql(q{SELECT max(test_chunk_put(test_noise('old ' || g, 20000)))
	FROM generate_series(1, 100) g});
$root = sql(q{SELECT test_chunk_put(test_noise('old 100', 20000))});
is(sql("SELECT test_chunk_move_root(NULL, '$root')"), 't', 'the root moved');
my $all_old = q{SELECT count(*) FROM generate_series(1, 100) g
	WHERE test_chunk_has(test_content_hash(test_noise('old ' || g, 20000)))};
is(sql($all_old), '100', 'every chunk found');
ok(!-e $file, 'no index file before a checkpoint');
sql('CHECKPOINT');
ok(-e $file, 'a checkpoint saves the index');

# What a backend reads of the journal to answer, after a start.
sub scanned
{
	my ($query) = @_;
	my @out = split /\n/, sql(qq{SELECT pg_stat_reset_shared('prolly');
		SELECT ($query) IS NOT NULL;
		SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = 'journal_bytes_scanned'});
	return $out[-1];
}

$node->restart;
cmp_ok(scanned($all_old), '<', 100000,
	'after a restart, the saved index answers: hardly any journal read');
is(sql($all_old), '100', 'every chunk found');
is(sql('SELECT test_chunk_root()'), $root, 'the root as it was');

# Appended after the file was saved, then a crash: found too, from the
# journal written since.
sql(q{SELECT count(test_chunk_put(test_noise('new ' || g, 20000)))
	FROM generate_series(1, 10) g});
$node->stop('immediate');
$node->start;
my $all_new = q{SELECT count(*) FROM generate_series(1, 10) g
	WHERE test_chunk_has(test_content_hash(test_noise('new ' || g, 20000)))};
my $read = scanned($all_new);
cmp_ok($read, '>=', 200000, 'after a crash, what came since is read');
cmp_ok($read, '<', 600000, 'and little else');
is(sql($all_new), '10', 'the chunks appended since are found');
is(sql($all_old), '100', 'and those saved');

# A damaged file is passed over: the journal is read whole.
sql('CHECKPOINT');
$node->stop;
open(my $fh, '+<:raw', $file) or die "open $file: $!";
seek($fh, 40, 0);
print $fh "\xff\xff\xff\xff";
close($fh);
$node->start;
cmp_ok(scanned($all_old), '>', 2000000, 'a damaged file: the journal read whole');
is(sql($all_old), '100', 'every chunk found');

# Saved, then a collection retires the segment the file names, then a
# crash before the next checkpoint: the file's chunks of that segment
# aren't taken.
sql('CHECKPOINT');
sql(q{SELECT kept > 0 FROM test_chunk_gc(false)});
$node->stop('immediate');
$node->start;
is(sql(qq{SELECT test_chunk_has('$root')}), 't', 'the root, kept, is found');
is(sql(q{SELECT test_chunk_has(test_content_hash(test_noise('old 1', 20000)))}), 'f',
	'a chunk collected isn\'t, though the file named it');
is(sql(q{SELECT test_chunk_get(test_content_hash(test_noise('old 1', 20000))) IS NULL}), 't',
	'nor read');

$node->stop;
done_testing();
