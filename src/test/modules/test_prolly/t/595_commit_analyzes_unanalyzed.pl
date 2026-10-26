# Copyright (c) 2026, PostgreSQL Global Development Group

# dolt_commit() analyzes the tables it commits that were never vacuumed or
# analyzed.  Without statistics, tables loaded, committed and queried at once
# (before autovacuum's first analyze) are planned from guesses: a CH-benCHmark
# join of customer, new_order, orders and order_line ran as nested loops over
# hundreds of millions of row pairs for more than ten minutes, where with
# statistics it takes a fraction of a second.
#
# A table already analyzed keeps its statistics, and an empty table, one
# whose autovacuum is off and one the committing user may not maintain are
# left alone.  Autovacuum is on but never wakes here, so no worker can
# analyze a table instead.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('commit_analyzes_unanalyzed');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = on
autovacuum_naptime = 2147483
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub counts
{
	my ($t) = @_;
	return sql(qq{SELECT reltuples::bigint || ' ' ||
				  (SELECT count(*) FROM pg_stats
				   WHERE schemaname = 'cs' AND tablename = '$t')
				  FROM pg_class WHERE oid = 'cs.$t'::regclass});
}

# A district's customers, its open orders and their lines, as TPC-C loads
# them: the join's keys repeat across tables, which default distinct
# counts take for nearly unique.
sql(q{
	CREATE SCHEMA cs VERSIONED;
	CREATE TABLE cs.customer (c_d_id int, c_id int, c_state char(2),
		PRIMARY KEY (c_d_id, c_id));
	CREATE TABLE cs.new_order (no_d_id int, no_o_id int,
		PRIMARY KEY (no_d_id, no_o_id));
	CREATE TABLE cs.orders (o_d_id int, o_id int, o_c_id int,
		PRIMARY KEY (o_d_id, o_id));
	CREATE TABLE cs.order_line (ol_d_id int, ol_o_id int, ol_number int,
		ol_amount numeric, PRIMARY KEY (ol_d_id, ol_o_id, ol_number));
	INSERT INTO cs.customer
		SELECT d, c, 'AK' FROM generate_series(1, 10) d,
			generate_series(1, 3000) c;
	INSERT INTO cs.orders
		SELECT d, o, (o * 7) % 3000 + 1 FROM generate_series(1, 10) d,
			generate_series(1, 3000) o;
	INSERT INTO cs.new_order
		SELECT d, o FROM generate_series(1, 10) d,
			generate_series(2101, 3000) o;
	INSERT INTO cs.order_line
		SELECT d, o, n, n FROM generate_series(1, 10) d,
			generate_series(1, 3000) o, generate_series(1, 10) n;

	CREATE TABLE cs.analyzed (id int PRIMARY KEY);
	INSERT INTO cs.analyzed SELECT generate_series(1, 300);
	ANALYZE cs.analyzed;
	DELETE FROM cs.analyzed WHERE id > 100;
	CREATE TABLE cs.off (id int PRIMARY KEY) WITH (autovacuum_enabled = false);
	INSERT INTO cs.off SELECT generate_series(1, 500);
	CREATE TABLE cs.empty (id int PRIMARY KEY);
	CREATE ROLE committer IN ROLE pg_prolly_write;
	GRANT USAGE, CREATE ON SCHEMA cs TO committer;
	GRANT EXECUTE ON FUNCTION dolt_commit(text[]) TO committer;
});

my $join = q{
	EXPLAIN SELECT ol.ol_o_id, ol.ol_d_id, sum(ol.ol_amount)
	FROM cs.customer c
	JOIN cs.new_order no ON no.no_d_id = c.c_d_id
	JOIN cs.orders o ON o.o_d_id = no.no_d_id AND o.o_id = no.no_o_id
		AND o.o_c_id = c.c_id
	JOIN cs.order_line ol ON ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id
	WHERE c.c_state >= 'A'
	GROUP BY ol.ol_o_id, ol.ol_d_id};

is(counts('order_line'), '-1 0', 'a table just loaded has no statistics');

sql(q{SELECT dolt_commit('-Am', 'load')});

is(counts('order_line'), '300000 4',
	'the commit analyzes a table never analyzed');
is(counts('customer'), '30000 3', 'and each other one');
is( sql(q{SELECT count(*) FROM pg_stat_user_tables
		  WHERE schemaname = 'cs' AND last_autoanalyze IS NOT NULL}),
	'0', 'no autovacuum worker analyzed them');
# (the join makes 90000 rows: guessed without statistics as about a tenth)
my ($rows) =
  sql($join) =~ /(?:Nested Loop|Hash Join|Merge Join)\s+\(cost=\S+ rows=(\d+)/;
cmp_ok($rows // 0, '>', 45000,
	"the join's estimate is the statistics', not default guesses");

is(counts('analyzed'), '300 1', 'an analyzed table keeps its statistics');
is(counts('off'), '-1 0', 'a table with autovacuum off is left alone');
is(counts('empty'), '-1 0', 'an empty table is left alone');

# Rows loaded into a table after its first commit: committed while the
# table was empty, the next commit analyzes it, and the one after leaves
# its statistics to autovacuum.
sql(q{
	CREATE TABLE cs.later (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'empty');
	INSERT INTO cs.later SELECT generate_series(1, 700);
	SELECT dolt_commit('-Am', 'later');
	INSERT INTO cs.later SELECT generate_series(701, 900);
	SELECT dolt_commit('-Am', 'more');
});
is(counts('later'), '700 1', 'a table analyzed keeps its statistics');

# A table the committing user may not maintain is left alone, without a
# warning.
sql(q{
	SET ROLE committer;
	CREATE TABLE cs.theirs (id int PRIMARY KEY);
	INSERT INTO cs.theirs SELECT generate_series(1, 50);
	RESET ROLE;
	CREATE TABLE cs.mine (id int PRIMARY KEY);
	INSERT INTO cs.mine SELECT generate_series(1, 50);
});
my ($stdout, $stderr);
$node->psql('postgres', q{
	SET ROLE committer;
	SELECT dolt_commit('-Am', 'two owners');
}, stdout => \$stdout, stderr => \$stderr, on_error_die => 1);
is($stderr, '', 'the commit warns of nothing');
is(counts('theirs'), '50 1', "the committer's own table is analyzed");
is(counts('mine'), '-1 0', 'one the committer may not maintain is not');

# On another branch, its tables are analyzed by its own commits.
sql(q{
	SELECT dolt_branch('side');
	SELECT dolt_checkout('side');
	CREATE TABLE cs.side_only (id int PRIMARY KEY);
	INSERT INTO cs.side_only SELECT generate_series(1, 1234);
	SELECT dolt_commit('-Am', 'side');
});
like(
	sql(q{
		SELECT dolt_checkout('side');
		SELECT reltuples::bigint FROM pg_class
		WHERE oid = 'cs.side_only'::regclass}),
	qr/\n1234\z/,
	"a branch's commit analyzes the branch's table");
is(sql(q{SELECT count(*) FROM dolt_status}), '0',
	'the analyzes leave the branch clean');

$node->stop;
done_testing();
