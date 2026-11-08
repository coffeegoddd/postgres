# Copyright (c) 2026, PostgreSQL Global Development Group

# A versioned schema moved in place between every ordered pair of the roots of a fixture touching
# every kind of value and most kinds of table, through one schema: after each move the tables hold
# the target, capture has it, each B-tree and GIN index agrees with its table, and each index of
# another kind answers what its table does.

use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $fixture = dirname(__FILE__) . '/../capture_fixture';
my $node = PostgreSQL::Test::Cluster->new('move_pairs');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION amcheck');

my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";

sub sql
{
	my ($sql) = @_;
	return $node->safe_psql('postgres', "$on$sql");
}

sub read_root { return sql(q{SELECT test_ws_root_capture('rt')}); }
sub capture_root { return sql(q{SELECT test_capture_barrier('rt')}); }

# The fixture's steps, each committed, so that its root is held.
sql(q{CREATE SCHEMA rt VERSIONED});
my @steps = ('load.sql', 'dml.sql', 'ddl.sql', 'dml_after.sql');
my @root;
for my $i (0 .. $#steps)
{
	sql(slurp_file("$fixture/$steps[$i]"));

	# (indexes of the kinds amcheck checks or a query must, from the first)
	sql(q{CREATE INDEX kinds_jb_gin ON rt.kinds USING gin (jb); CREATE INDEX kinds_pt_gist ON rt.kinds USING gist (pt)})
	  if $i == 0;
	$root[$i] = sql(qq{SELECT test_capture_commit_on('rt', 'main', '$steps[$i]')});
	is($root[$i], read_root(), "$steps[$i]: committed as the tables are");
}

# How many B-tree and GIN indexes are checked, each with its heap; amcheck raises for one that isn't right.
sub indexes_check
{
	return sql(q{SELECT count(*) FROM (SELECT c.oid, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
		WHERE c.relnamespace = 'rt'::regnamespace AND c.relkind = 'i' AND a.amname IN ('btree', 'gin') OFFSET 0) x
		WHERE CASE x.amname WHEN 'btree' THEN bt_index_check(x.oid, true) ELSE gin_index_check(x.oid) END IS NOT NULL});
}

# What's wrong in each table's heap, and its TOAST table's.
sub heaps_check
{
	return sql(q{SELECT count(*) FROM (SELECT r.oid FROM pg_class c,
		LATERAL (VALUES (c.oid), (nullif(c.reltoastrelid, 0))) r(oid)
		WHERE c.relnamespace = 'rt'::regnamespace AND c.relkind IN ('r', 'm') AND r.oid IS NOT NULL OFFSET 0) x,
		LATERAL verify_heapam(x.oid)});
}

# What a query answers through an index, the plan using it, and with no index at all.
my $index_off = 'SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off;';
my $seq_off = 'SET enable_seqscan = off;';
my @through = (
	[ 'kinds_extra_hash', q{SELECT count(*), sum(a) FROM rt.kinds WHERE extra = 42} ],
	[ 'kinds_d_brin', q{SELECT count(*), sum(a) FROM rt.kinds WHERE d > '2021-06-01'} ],
	[ 'node_path_spgist', q{SELECT count(*), sum(id) FROM rt.node WHERE path ^@ 'p12'} ],
	[ 'kinds_pt_gist', q{SELECT count(*), sum(a) FROM rt.kinds WHERE pt <@ box(point(10, 0), point(20, 5))} ]);

sub through_indexes
{
	my ($name) = @_;
	for my $t (@through)
	{
		my ($index, $query) = @$t;
		next if sql(qq{SELECT to_regclass('rt.$index') IS NULL}) eq 't';
		like(sql("$seq_off EXPLAIN (COSTS OFF) $query"), qr/\b$index\b/, "$name: $index is used");
		is(sql("$seq_off $query"), sql("$index_off $query"), "$name: $index answers what its table does");
	}
}

# Every ordered pair once, from the last step: 3-0-1-2-3-1-0-2-1-3-2-0-3.
my @order = (3, 0, 1, 2, 3, 1, 0, 2, 1, 3, 2, 0, 3);
my %done;
for my $k (1 .. $#order)
{
	my ($from, $to) = ($order[ $k - 1 ], $order[$k]);
	my $name = "$steps[$from] to $steps[$to]";
	$done{"$from$to"}++;
	sql(qq{SELECT test_ws_move_prepare('rt', '$root[$to]')});
	my $stats = sql(qq{SELECT test_ws_move('rt', '$root[$to]')});
	note("$name: $stats");
	is(read_root(), $root[$to], "$name: the tables hold the root");
	is(capture_root(), $root[$to], "$name: and capture has it");
	cmp_ok(indexes_check(), '>', 10, "$name: each B-tree and GIN index agrees with its table");
	is(heaps_check(), '0', "$name: and each heap is sound");
	through_indexes($name);
}
is(scalar(keys %done), 12, 'every ordered pair is moved between once');

unlike(slurp_file($node->logfile), qr/\b(ERROR|FATAL|PANIC):|terminated by signal/,
	'nothing went wrong, capture\'s worker included');

done_testing();
