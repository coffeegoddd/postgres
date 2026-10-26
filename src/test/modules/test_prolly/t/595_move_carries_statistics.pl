# Copyright (c) 2026, PostgreSQL Global Development Group

# Planner statistics across moves.  A branch's first use copies its source's
# relations with their statistics, and a reset keeps a table's own; a table
# a merge or a pick of another branch's head makes here is loaded from that
# branch's rows, and takes that branch's statistics of it.  Without them the
# planner guesses at the new table (its index builds count its rows, but no
# column has statistics) until autovacuum analyzes it, and joins of such
# tables can be planned as nested loops over millions of pairs of rows.
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

$node->stop;
done_testing();
