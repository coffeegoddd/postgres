# Copyright (c) 2026, PostgreSQL Global Development Group

# Planner statistics across moves.  A branch's first use copies its source's
# relations with their statistics, and a reset keeps a table's own; a table
# a merge or a pick of another branch's head makes here is loaded from that
# branch's rows, and takes that branch's statistics of it.  Without them the
# planner guesses at the new table (its index builds count its rows, but no
# column has statistics) until autovacuum analyzes it, and joins of such
# tables can be planned as nested loops over millions of pairs of rows.
# Its expression indexes', extended statistics' and inherited statistics
# come too, and columns of an enum or domain the move made again; a
# source that renamed the table's columns since its head gives none.
#
# Autovacuum is on but never wakes here, so no worker can analyze a table.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move_carries_statistics');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = on
autovacuum_naptime = 2147483
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# (on branch: a checkout's output comes first, then the query's last line)
sub on
{
	my ($branch, $query) = @_;
	my @out = split /\n/,
	  sql(qq{SELECT dolt_checkout('$branch'); $query});
	return $out[-1];
}

# A table's statistics on branch: its row count and columns with statistics.
sub stats
{
	my ($branch, $t) = @_;
	return on($branch, qq{
		SELECT reltuples::bigint || ' ' ||
			(SELECT count(*) FROM pg_stats
			 WHERE schemaname = 's' AND tablename = '$t')
		FROM pg_class WHERE oid = 's.$t'::regclass});
}

# The planner's estimate of the rows of one of t's seven values of v.
sub estimate
{
	my ($branch, $t) = @_;
	my ($rows) = sql(qq{SELECT dolt_checkout('$branch');
		EXPLAIN SELECT * FROM s.$t WHERE v = 3}) =~ /rows=(\d+)/;
	return $rows;
}

sub load
{
	my ($t) = @_;
	return qq{
		CREATE TABLE s.$t (id int PRIMARY KEY, v int, k text);
		INSERT INTO s.$t
			SELECT g, g % 7, 'k' || (g % 13) FROM generate_series(1, 7000) g;
		ANALYZE s.$t;
	};
}

sql(q{CREATE SCHEMA s VERSIONED;} . load('t') . q{
	SELECT dolt_commit('-Am', 'base');
	SELECT dolt_branch('b');});

is(stats('b', 't'), '7000 3', "a branch's first use copies the statistics");
on('b', q{
	UPDATE s.t SET v = v WHERE id <= 10;
	SELECT dolt_reset('--hard', 'HEAD')});
is(stats('b', 't'), '7000 3', "a reset keeps the table's statistics");

# A table new on b, merged into main at b's head (a fast-forward).
on('b', load('ff') . q{SELECT dolt_commit('-Am', 'ff')});
on('main', q{SELECT dolt_merge('b')});
is(stats('main', 'ff'), '7000 3',
	'a table a fast-forward merge makes takes the branch\'s statistics');
cmp_ok(estimate('main', 'ff'), '>', 700,
	"the planner's estimate follows them");

# One new on b while main moved on too (a merge commit).
on('main', q{
	CREATE TABLE s.other (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'main moves')});
on('b', load('merged') . q{SELECT dolt_commit('-Am', 'merged')});
on('main', q{SELECT dolt_merge('b')});
is(stats('main', 'merged'), '7000 3',
	'a table a three-way merge makes takes them');
cmp_ok(estimate('main', 'merged'), '>', 700, 'and is estimated by them');
is(on('main', q{SELECT count(*) FROM dolt_status}), '0',
	'the merge leaves the branch clean');

# One picked from b's head.
on('b', load('picked') . q{SELECT dolt_commit('-Am', 'picked')});
on('main', q{SELECT dolt_cherry_pick('b')});
is(stats('main', 'picked'), '7000 3', 'a table a pick makes takes them');

# One b never analyzed: nothing to carry, the move's index builds count it.
on('b', q{
	CREATE TABLE s.raw (id int PRIMARY KEY, v int);
	INSERT INTO s.raw SELECT g, g % 7 FROM generate_series(1, 700) g;
	SELECT dolt_commit('-Am', 'raw')});
on('main', q{SELECT dolt_merge('b')});
is(stats('main', 'raw'), '700 0', 'one never analyzed has none to take');

# What else a table's statistics are: its expression indexes', its
# extended statistics' data, a partitioned table's inherited rows, and its
# columns' of versioned types the move makes again (other OIDs): an enum's
# (its values label OIDs, mapped label to label) and a domain's.
sql(q{
	CREATE TYPE s.e AS ENUM ('a', 'b');
	SELECT dolt_commit('-Am', 'enum');
	SELECT dolt_checkout('b');
	SELECT dolt_merge('main');
	ALTER TYPE s.e ADD VALUE 'z';
	CREATE DOMAIN s.d AS int CHECK (VALUE > 0);
	CREATE TABLE s.ex (id int PRIMARY KEY, k text, x s.e, dd s.d, a int, b int);
	INSERT INTO s.ex
		SELECT g, 'k' || (g % 13),
			CASE WHEN g % 7 = 0 THEN 'z'::s.e WHEN g % 2 = 0 THEN 'a' ELSE 'b' END,
			g % 5 + 1, g % 3, g % 3
		FROM generate_series(1, 7000) g;
	CREATE INDEX ex_lower ON s.ex (lower(k));
	CREATE STATISTICS s.ex_ab (ndistinct) ON a, b FROM s.ex;
	CREATE TABLE s.part (id int, v int) PARTITION BY RANGE (id);
	CREATE TABLE s.part1 PARTITION OF s.part FOR VALUES FROM (0) TO (5000);
	CREATE TABLE s.part2 PARTITION OF s.part FOR VALUES FROM (5000) TO (10000);
	INSERT INTO s.part SELECT g, g % 7 FROM generate_series(1, 9000) g;
	ANALYZE s.ex;
	ANALYZE s.part;
	SELECT dolt_commit('-Am', 'kinds');
});
isnt(on('b', q{SELECT 's.e'::regtype::oid}),
	on('main', q{SELECT dolt_merge('b'); SELECT 's.e'::regtype::oid}),
	'the merge made the enum again');
sub plan_rows
{
	my ($query) = @_;
	my ($rows) =
	  sql(qq{SELECT dolt_checkout('main'); EXPLAIN $query}) =~ /rows=(\d+)/;
	return $rows;
}
cmp_ok(plan_rows(q{SELECT * FROM s.ex WHERE x = 'z'}), '>', 700,
	"an enum column's statistics are carried, its labels mapped");
cmp_ok(plan_rows(q{SELECT * FROM s.ex WHERE dd = 2}), '>', 1000,
	"a domain column's are");
cmp_ok(plan_rows(q{SELECT * FROM s.ex WHERE lower(k) = 'k3'}), '>', 400,
	"an expression index's are");
is( on('main', q{SELECT n_distinct FROM pg_stats_ext
		WHERE statistics_name = 'ex_ab'}),
	'[{"attributes": [5, 6], "ndistinct": 3}]',
	"an extended statistics object's data is");
is( on('main', q{SELECT count(*) FROM pg_stats
		WHERE tablename = 'part' AND inherited}),
	'2', "a partitioned table's inherited rows are");

# Statistics whose values are of another type than the column's: an array
# column's elements and their counts, a tsvector's lexemes, a range's
# lengths, copied as they are.  Arrays of the enum and of the domain the
# move made again are carried as their elements are, an enum array's labels
# mapped in its most common elements and in its whole arrays; a column of
# a composite type the move made again takes none, and the table's other
# columns still do.
sql(q{
	SELECT dolt_checkout('b');
	CREATE TYPE s.c AS (x int, y text);
	CREATE TABLE s.arr (id int PRIMARY KEY, ia int[], tv tsvector,
		r int4range, ee s.e[], dda s.d[], cc s.c);
	INSERT INTO s.arr
		SELECT g, ARRAY[g % 10, g % 7], to_tsvector('simple', 'w' || (g % 11)),
			int4range(g % 50, g % 50 + g % 9 + 1),
			ARRAY[CASE WHEN g % 3 = 0 THEN 'z' WHEN g % 3 = 1 THEN 'a'
				  ELSE 'b' END]::s.e[],
			ARRAY[g % 4 + 1, g % 6 + 1]::s.d[],
			ROW(g % 5, 'c' || (g % 5))::s.c
		FROM generate_series(1, 5000) g;
	ANALYZE s.arr;
	SELECT dolt_commit('-Am', 'arr');
});
on('main', q{SELECT dolt_merge('b')});
is( on('main', q{
		SELECT string_agg(attname || ':' ||
			(most_common_vals IS NOT NULL OR most_common_elems IS NOT NULL OR
			 range_length_histogram IS NOT NULL OR histogram_bounds IS NOT NULL),
			',' ORDER BY attname)
		FROM pg_stats WHERE schemaname = 's' AND tablename = 'arr'}),
	'dda:true,ee:true,ia:true,id:true,r:true,tv:true',
	"array, tsvector, range and versioned arrays' statistics are carried and read");
is( on('main', q{SELECT most_common_elems::text || ' ' || most_common_vals::text
		FROM pg_stats WHERE tablename = 'arr' AND attname = 'ee'}),
	on('b', q{SELECT most_common_elems::text || ' ' || most_common_vals::text
		FROM pg_stats WHERE tablename = 'arr' AND attname = 'ee'}),
	"an enum array's statistics read as the source's");
cmp_ok(plan_rows(q{SELECT * FROM s.arr WHERE ee @> ARRAY['z'::s.e]}), '>', 1000,
	"an enum array's elements are estimated from them, labels mapped");
cmp_ok(plan_rows(q{SELECT * FROM s.arr WHERE ee = ARRAY['a'::s.e]}), '>', 1000,
	"and its whole arrays");
cmp_ok(plan_rows(q{SELECT * FROM s.arr WHERE dda @> ARRAY[2]::s.d[]}), '>', 1000,
	"a domain array's elements are");
like(
	on('main', q{SELECT most_common_elems::text FROM pg_stats
		WHERE tablename = 'arr' AND attname = 'ia'}),
	qr/^\{0,1,2,3,4,5,6,7,8,9\}$/,
	"an array column's elements read as the source has them");
cmp_ok(plan_rows(q{SELECT * FROM s.arr WHERE ia @> ARRAY[3]}), '>', 500,
	'@> is estimated from them');
cmp_ok(plan_rows(q{SELECT * FROM s.arr WHERE r && int4range(10, 12)}), '>', 50,
	'&& is estimated from them');
is( on('main', q{SELECT count(*) FROM s.arr WHERE ia @> ARRAY[3]
		AND tv @@ 'w3'::tsquery AND r && int4range(0, 100)}),
	on('b', q{SELECT count(*) FROM s.arr WHERE ia @> ARRAY[3]
		AND tv @@ 'w3'::tsquery AND r && int4range(0, 100)}),
	'and the table reads as the source does');

# A source whose working set renamed its columns since its head: its
# statistics describe other columns than the ones of the same name the
# move made, and none are carried.
sql(q{
	SELECT dolt_checkout('b');
	CREATE TABLE s.sw (id int PRIMARY KEY, a int, b int);
	INSERT INTO s.sw SELECT g, g % 7, g % 100 FROM generate_series(1, 7000) g;
	ANALYZE s.sw;
	SELECT dolt_commit('-Am', 'sw');
	ALTER TABLE s.sw RENAME a TO tmp;
	ALTER TABLE s.sw RENAME b TO a;
	ALTER TABLE s.sw RENAME tmp TO b;
	ANALYZE s.sw;
});
on('main', q{SELECT dolt_merge('b')});
is(stats('main', 'sw'), '7000 0',
	"columns renamed since the source's head carry nothing");

# A table identity conflict resolved with --theirs makes their relation
# here, which takes their statistics.
sql(q{
	SELECT dolt_checkout('main');
	CREATE TABLE s.ic (id int PRIMARY KEY, v int);
	SELECT dolt_commit('-Am', 'ic');
	SELECT dolt_branch('old');
	SELECT dolt_checkout('old');
	INSERT INTO s.ic SELECT g, g % 7 FROM generate_series(1, 7000) g;
	ANALYZE s.ic;
	SELECT dolt_commit('-Am', 'old rows');
	SELECT dolt_checkout('main');
	DROP TABLE s.ic;
	SELECT dolt_commit('-Am', 'dropped');
	CREATE TABLE s.ic (id int PRIMARY KEY, v int);
	INSERT INTO s.ic VALUES (1, 1);
	SELECT dolt_commit('-Am', 'made again');
});
my @resolved = split /\n/, sql(q{
	SELECT dolt_checkout('main');
	BEGIN;
	SELECT count(*) FROM dolt_merge('old') m;
	SELECT count(*) FROM dolt_identity_conflicts();
	SELECT dolt_conflicts_resolve('--theirs', 's.ic') IS NOT NULL;
	SELECT count(*) || ' ' || (SELECT count(*) FROM pg_stats
		WHERE schemaname = 's' AND tablename = 'ic') FROM s.ic;
	SELECT dolt_commit('-m', 'took theirs') IS NOT NULL;
	COMMIT;
});
is($resolved[2], '1', 'the merge stops on a table identity conflict');
is($resolved[4], '7000 2',
	'the table --theirs makes takes their statistics');

$node->stop;
done_testing();
