# Copyright (c) 2026, PostgreSQL Global Development Group

# A retire's cost is what it retires, not the store's size: for the same
# churn per batch, the work counters of a retire (chunk-file reads, tree
# nodes read, page-store blocks scanned for hash pointers, the history
# horizon's entries read, journal bytes scanned, table files' indexes
# probed) are the same, within 20%, on a small store and on one with
# several times more settled data and table files, all older than what
# is retired, with the same history.  Counted, not timed.
#
# Three of the counters are pg_stat_prolly's to come: gc_chunks_read, the
# chunks a collection reads to mark what they name (from the files it
# collects, or, a retire's, from settled files it reads through);
# gc_xrefs_read, the entries of settled files' cross-generation sections a
# retire reads; chunk_index_probes, the table files' indexes a lookup of a
# chunk probes, each file whose index it reads counting once.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('gen_retire_cost');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
prolly.autofold_threshold = 0
});
$node->start;

# (what pg_stat_prolly counts of the work, and, last, the counters no
# collection's reads move yet: the chunks it reads to mark what they name,
# the section entries a retire reads, and the indexes probed)
my @COUNTERS = qw(chunk_file_reads nodes_read refs_nodes_read gc_pages_scanned
  horizon_entries_read journal_bytes_scanned commits_read gc_chunks_kept
  gc_chunks_freed gc_bytes_copied table_file_bytes_written
  gc_chunks_read gc_xrefs_read chunk_index_probes);

sub sql { my ($db, $q) = @_; return $node->safe_psql($db, $q); }

# The counters a statement's work moved, in one session: reset, the
# statement, flushed, read.
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

sub setup
{
	my ($db, $big) = @_;
	sql('postgres', qq{CREATE DATABASE $db});
	sql($db, q{CREATE EXTENSION test_prolly;
		CREATE FUNCTION day(d int) RETURNS timestamptz LANGUAGE sql AS
			$$ SELECT timestamptz '2020-01-01 00:00+00' + make_interval(days => d) $$;
		CREATE FUNCTION pad(id int) RETURNS text LANGUAGE sql IMMUTABLE AS
			$$ SELECT string_agg(md5(k || ':' || id), '') FROM generate_series(1, 30) k $$;
		CREATE SCHEMA v VERSIONED;
		CREATE TABLE v.t (id int PRIMARY KEY, rev int NOT NULL, pad text NOT NULL);
		INSERT INTO v.t SELECT g, 0, pad(g) FROM generate_series(1, 1000) g;
		CREATE TABLE v.big (id int PRIMARY KEY, rev int NOT NULL, pad text NOT NULL)});
	# the larger store: six times the rows in a table of their own, then a
	# dozen batches of churn on it, each settled in a file of its own; the
	# smaller makes as many commits, empty, settled together (both have
	# the same history to sever)
	sql($db, qq{INSERT INTO v.big SELECT g, 0, pad(-g) FROM generate_series(1, $big) g});
	sql($db, q{SELECT dolt_commit('-Am', 'c0', '--date', day(0)::text)});
	sql($db, q{SELECT * FROM test_chunk_gc(false)});
	for my $j (1 .. 12)
	{
		if ($big > 0)
		{
			sql($db, qq{UPDATE v.big SET rev = $j WHERE id % 20 = $j % 20});
			sql($db, qq{SELECT dolt_commit('-am', 'b$j', '--date', day($j)::text)});
			sql($db, q{SELECT * FROM test_chunk_gc(false)});
		}
		else
		{
			sql($db, qq{SELECT dolt_commit('--allow-empty', '-m', 'b$j', '--date',
				day($j)::text)});
		}
	}
	sql($db, q{SELECT * FROM test_chunk_gc(false)});
}

# The same churn on v.t in both: collect, sever all but the newest three
# commits, collect what the sever wrote (so the retire's young set is
# nothing in either), and retire the generation of the commit just
# severed; the counters of the last such retire.
sub batches
{
	my ($db) = @_;
	my (%gen, $c, $out);
	for my $i (1 .. 5)
	{
		my $d = 100 + $i;
		sql($db, qq{UPDATE v.t SET rev = $i WHERE id % 20 = $i % 20});
		sql($db, qq{SELECT dolt_commit('-am', 'c$i', '--date', day($d)::text)});
		sql($db, q{SELECT * FROM test_chunk_gc(false)});
		$gen{$i} = sql($db, q{SELECT test_chunk_generation()});
		next if $i < 3;
		sql($db, sprintf(q{SELECT dolt_history_drop(day(%d))}, $d - 2));
		sql($db, q{SELECT * FROM test_chunk_gc(false)});
		my $g = $gen{ $i - 2 };
		$c = counted($db, qq{SELECT * FROM test_chunk_retire($g, $g)});
	}
	return $c;
}

setup('small', 0);
setup('large', 6000);
my ($sf, $sb) = split /\|/, sql('small', q{SELECT count(*), sum(bytes)
	FROM test_chunk_table_files() WHERE settled});
my ($lf, $lb) = split /\|/, sql('large', q{SELECT count(*), sum(bytes)
	FROM test_chunk_table_files() WHERE settled});
note("settled before the batches: small $sf files, $sb bytes; large $lf files, $lb bytes");
cmp_ok($lb, '>=', 4 * $sb, 'the larger store holds several times the settled bytes');
cmp_ok($lf, '>=', 4 * $sf, 'and several times the settled files');

my $small = batches('small');
my $large = batches('large');
note('small: ' . join(' ', map { "$_=$small->{$_}" } @COUNTERS));
note('large: ' . join(' ', map { "$_=$large->{$_}" } @COUNTERS));
for my $c (@COUNTERS)
{
	isnt($small->{$c}, 'none', "$c is counted");
	next if $small->{$c} eq 'none' || $large->{$c} eq 'none';
	# (within 20%, and a couple of units for counts that small)
	my $slack = 0.2 * $small->{$c} > 2 ? 0.2 * $small->{$c} : 2;
	cmp_ok(abs($large->{$c} - $small->{$c}), '<=', $slack,
		"a retire's $c is the same on the larger store ($small->{$c} small, $large->{$c} large)");
}
# (what it retires is read, and what newer files list in it: the counts
# move at all)
cmp_ok($small->{gc_chunks_kept}, '>', 0, 'the retire keeps what is live');
for my $c (qw(gc_chunks_read gc_xrefs_read chunk_index_probes))
{
	ok($small->{$c} ne 'none' && $small->{$c} > 0, "the retire's $c are counted");
}

$node->stop;
done_testing();
