# Copyright (c) 2026, PostgreSQL Global Development Group

# A default collection costs its young set, not the number of table
# files: for the same young set, its work counters (the chunks it reads,
# the table files' indexes its lookups probe, recording each reference
# its copies make in a generation included, and the chunk-file reads)
# are the same, within 20%, on a store of ten settled files and on one of
# over a hundred.  Counted, not timed.
#
# Two of the counters are pg_stat_prolly's to come: gc_chunks_read, the
# chunks a collection reads to mark what they name; chunk_index_probes,
# the table files' indexes a lookup of a chunk probes, each file whose
# index it reads counting once (a collection's lookups, and recording's,
# included).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('gen_default_gc_cost');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
prolly.autofold_threshold = 0
});
$node->start;

my @COUNTERS = qw(chunk_file_reads journal_bytes_scanned gc_chunks_kept
  gc_bytes_copied table_file_bytes_written gc_chunks_read chunk_index_probes);

sub sql { my ($db, $q) = @_; return $node->safe_psql($db, $q); }

sub counted
{
	my ($db, $q) = @_;
	my $names = join(',', map { "'$_'" } @COUNTERS);
	my $out = sql($db, qq{
		SELECT pg_stat_force_next_flush();
		SELECT pg_stat_reset_shared('prolly');
		SELECT pg_stat_force_next_flush();
		$q;
		SELECT pg_stat_force_next_flush();
		SELECT 'counted ' || string_agg(c.name || '=' || coalesce(s.value::text, 'none'), ' ')
		FROM unnest(ARRAY[$names]) c(name)
		LEFT JOIN pg_stat_prolly s ON s.name = c.name});
	my ($line) = $out =~ /^counted (.*)$/m;
	my %c = map { split /=/ } split / /, $line;
	return \%c;
}

# A store of the same table, with files settled commits on another table
# made, each collected on its own.
sub setup
{
	my ($db, $files) = @_;
	sql('postgres', qq{CREATE DATABASE $db});
	sql($db, q{CREATE EXTENSION test_prolly;
		CREATE FUNCTION pad(id int) RETURNS text LANGUAGE sql IMMUTABLE AS
			$$ SELECT string_agg(md5(k || ':' || id), '') FROM generate_series(1, 30) k $$;
		CREATE SCHEMA v VERSIONED;
		CREATE TABLE v.t (id int PRIMARY KEY, rev int NOT NULL, pad text NOT NULL);
		INSERT INTO v.t SELECT g, 0, pad(g) FROM generate_series(1, 2000) g;
		CREATE TABLE v.side (id int PRIMARY KEY, rev int NOT NULL, pad text NOT NULL);
		INSERT INTO v.side SELECT g, 0, pad(-g) FROM generate_series(1, 200) g;
		SELECT dolt_commit('-Am', 'base')});
	sql($db, q{SELECT * FROM test_chunk_gc(false)});
	sql($db, join("\n", map { qq{UPDATE v.side SET rev = $_ WHERE id = $_ % 200 + 1;
		SELECT dolt_commit('-am', 's$_');
		SELECT * FROM test_chunk_gc(false);} } 1 .. $files));
	return sql($db, q{SELECT count(*) FROM test_chunk_table_files() WHERE settled});
}

# The young set: the same rows of v.t updated and committed, a row in a
# hundred, so the nodes above them, written anew, name the many leaves
# left as they were, all in the oldest file (each a reference recording
# places in its generation); then the default collection, counted.
sub measured
{
	my ($db) = @_;
	sql($db, q{UPDATE v.t SET rev = 1 WHERE id % 100 = 0});
	sql($db, q{SELECT dolt_commit('-am', 'measured')});
	my $young = sql($db, q{SELECT journal_bytes FROM prolly_store_sizes()});
	my $c = counted($db, q{SELECT * FROM test_chunk_gc(false)});
	$c->{young} = $young;
	return $c;
}

my $nfew = setup('few', 9);
my $nmany = setup('many', 110);
note("settled files: few $nfew, many $nmany");
cmp_ok($nfew, '<=', 12, 'the small store has about ten settled files');
cmp_ok($nmany, '>=', 100, 'the other over a hundred');

my $few = measured('few');
my $many = measured('many');
note('few: ' . join(' ', map { "$_=$few->{$_}" } ('young', @COUNTERS)));
note('many: ' . join(' ', map { "$_=$many->{$_}" } ('young', @COUNTERS)));
cmp_ok(abs($many->{young} - $few->{young}), '<=', 0.2 * $few->{young},
	"the same young set ($few->{young} bytes, $many->{young})");
for my $c (@COUNTERS)
{
	isnt($few->{$c}, 'none', "$c is counted");
	next if $few->{$c} eq 'none' || $many->{$c} eq 'none';
	my $slack = 0.2 * $few->{$c} > 2 ? 0.2 * $few->{$c} : 2;
	cmp_ok(abs($many->{$c} - $few->{$c}), '<=', $slack,
		"a default collection's $c is the same with $nmany files as with $nfew "
		  . "($few->{$c}, $many->{$c})");
}
cmp_ok($few->{gc_chunks_kept}, '>', 0, 'the collection keeps what is live');
for my $c (qw(gc_chunks_read chunk_index_probes))
{
	ok($few->{$c} ne 'none' && $few->{$c} > 0, "the collection's $c are counted");
}

$node->stop;
done_testing();
