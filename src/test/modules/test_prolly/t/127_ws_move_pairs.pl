# Copyright (c) 2026, PostgreSQL Global Development Group

# A versioned schema moved in place between every ordered pair of the roots of a fixture touching
# every kind of value and most kinds of table, through one schema.  After each move the tables hold
# the target and capture has it; amcheck passes on each heap and each B-tree index with its heap,
# and on each GIN index; each index amcheck can't hold to its heap answers through a plan using it
# what its table does; the constraints refuse what they refuse; and the rows the move wrote are
# what the change counters say, and what a logical slot is sent.

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

# How many B-tree and GIN indexes are checked: a B-tree with its heap, and unique; amcheck raises
# for one that isn't right.
sub indexes_check
{
	return sql(q{SELECT count(*) FROM (SELECT c.oid, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
		WHERE c.relnamespace = 'rt'::regnamespace AND c.relkind = 'i' AND a.amname IN ('btree', 'gin')
		OFFSET 0) x
		WHERE CASE x.amname WHEN 'btree' THEN bt_index_check(x.oid, true, true)
			ELSE gin_index_check(x.oid) END IS NOT NULL});
}

# What's wrong in each table's heap, and its TOAST table's.
sub heaps_check
{
	return sql(q{SELECT count(*) FROM (SELECT r.oid FROM pg_class c,
		LATERAL (VALUES (c.oid), (nullif(c.reltoastrelid, 0))) r(oid)
		WHERE c.relnamespace = 'rt'::regnamespace AND c.relkind IN ('r', 'm') AND r.oid IS NOT NULL
		OFFSET 0) x,
		LATERAL verify_heapam(x.oid)});
}

# What a query answers through an index, the plan using it, and with no index at all: each index of
# a kind amcheck can't hold to its heap, and the GIN index, whose check is of the index alone.
my $index_off = 'SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off;';
my $seq_off = 'SET enable_seqscan = off;';
my @through = (
	[ 'kinds_extra_hash', q{SELECT count(*), sum(a) FROM rt.kinds WHERE extra = 42} ],
	[ 'kinds_d_brin', q{SELECT count(*), sum(a) FROM rt.kinds WHERE d > '2021-06-01'} ],
	[ 'node_path_spgist', q{SELECT count(*), sum(id) FROM rt.node WHERE path ^@ 'p12'} ],
	[ 'kinds_pt_gist', q{SELECT count(*), sum(a) FROM rt.kinds WHERE pt <@ box(point(10, 0), point(20, 5))} ],
	[ 'kinds_jb_gin', q{SELECT count(*), sum(a) FROM rt.kinds WHERE jb ? 'upd'} ]);

sub indexes_there
{
	return join(',', grep { sql(qq{SELECT to_regclass('rt.$_') IS NOT NULL}) eq 't' } map { $_->[0] } @through);
}

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

# What each constraint refuses, by its SQLSTATE: anything else, or nothing, is an error.  (None takes
# a sequence's value, which no rollback gives back.)
my @refused = (
	[ '23505', q{INSERT INTO rt.kinds (a, b) SELECT a, b FROM rt.kinds LIMIT 1} ],
	[ '23503', q{INSERT INTO rt.ledger VALUES (-1, 1, 1)} ],
	[ '23503', q{SET CONSTRAINTS ALL IMMEDIATE; DELETE FROM rt.node WHERE id = 1} ],
	[ '23514', q{INSERT INTO rt.acct (id, email, score) OVERRIDING SYSTEM VALUE VALUES (-1, 'new@x', 101)} ],
	[ '23514', q{INSERT INTO rt.kinds (a, b, dom) VALUES (7777, 'q', 0)} ],
	[ '23502', q{INSERT INTO rt.acct (id, email) OVERRIDING SYSTEM VALUE VALUES (-1, NULL)} ],
	[ '428C9', q{INSERT INTO rt.acct (id, email) VALUES (1, 'a')} ]);

sub constraints_refuse
{
	my ($name) = @_;
	for my $r (@refused)
	{
		my ($state, $stmt) = @$r;
		my ($rc, $out, $err) = $node->psql('postgres', qq{$on DO \$\$BEGIN $stmt; RAISE EXCEPTION 'allowed';
			EXCEPTION WHEN SQLSTATE '$state' THEN NULL; END\$\$});
		is($rc, 0, "$name: refused with $state: $stmt") or diag($err);
	}
}

# Each table's changes since it was analyzed, by its OID.
sub changes
{
	my %n = map { split /\|/ } split /\n/, sql(q{SELECT relid, n_mod_since_analyze FROM pg_stat_user_tables
		WHERE schemaname = 'rt'});
	return \%n;
}

# The fixture's steps, each committed, so that its root is held, with what each has of indexes.
sql(q{CREATE SCHEMA rt VERSIONED});
my @steps = ('load.sql', 'dml.sql', 'ddl.sql', 'dml_after.sql');
my (@root, @nindexes, @there);
for my $i (0 .. $#steps)
{
	sql(slurp_file("$fixture/$steps[$i]"));

	# (indexes of the kinds amcheck checks or a query must, from the first)
	sql(q{CREATE INDEX kinds_jb_gin ON rt.kinds USING gin (jb); CREATE INDEX kinds_pt_gist ON rt.kinds USING gist (pt)})
	  if $i == 0;
	$root[$i] = sql(qq{SELECT test_capture_commit_on('rt', 'main', '$steps[$i]')});
	is($root[$i], read_root(), "$steps[$i]: committed as the tables are");
	$nindexes[$i] = indexes_check();
	$there[$i] = indexes_there();
}

# Every ordered pair once, from the last step: 3-0-1-2-3-1-0-2-1-3-2-0-3.
my @order = (3, 0, 1, 2, 3, 1, 0, 2, 1, 3, 2, 0, 3);
my %done;
for my $k (1 .. $#order)
{
	my ($from, $to) = ($order[ $k - 1 ], $order[$k]);
	my $name = "$steps[$from] to $steps[$to]";
	$done{"$from$to"}++ if $from != $to;
	sql(qq{SELECT test_ws_move_prepare('rt', '$root[$to]')});
	my $before = changes();
	sql(q{SELECT pg_create_logical_replication_slot('watch', 'test_decoding')});
	my $stats = sql(qq{SELECT test_ws_move('rt', '$root[$to]'); SELECT pg_stat_force_next_flush()});
	note("$name: $stats");
	is(read_root(), $root[$to], "$name: the tables hold the root");
	is(capture_root(), $root[$to], "$name: and capture has it");

	# (the move's session sent its counts before it said it was done)
	# (as bytes: a value it prints may hold a zero byte, and only how a line starts is asked)
	my $sent = sql(q{SELECT count(*) FROM pg_logical_slot_get_binary_changes('watch', NULL, NULL)
		WHERE encode(substring(data FROM 1 FOR 200), 'escape') ~ '^table rt\.[^:]+: (INSERT|UPDATE|DELETE):'});
	sql(q{SELECT pg_drop_replication_slot('watch')});
	my $after = changes();
	my ($kept, $made) = (0, 0);
	for my $relid (keys %$after)
	{
		if (exists $before->{$relid}) { $kept += $after->{$relid} - $before->{$relid}; }
		else { $made += $after->{$relid}; }
	}
	my ($deleted, $inserted) = $stats =~ /deleted (\d+) inserted (\d+)/;
	is($kept, $deleted + $inserted, "$name: a kept table's counts are of the rows the move wrote");
	is($sent, $kept + $made, "$name: a logical slot is sent each row written, a table's made too");

	is(indexes_check(), $nindexes[$to], "$name: each B-tree and GIN index the target has is sound");
	is(heaps_check(), '0', "$name: and each heap");
	is(indexes_there(), $there[$to], "$name: the indexes a query checks are the target's");
	through_indexes($name);
	constraints_refuse($name);
	is(read_root(), $root[$to], "$name: what is refused leaves the tables as they were");
}
is(scalar(keys %done), 12, 'every ordered pair is moved between once');

unlike(slurp_file($node->logfile), qr/\b(ERROR|FATAL|PANIC):|terminated by signal/,
	'nothing went wrong, capture\'s worker included');

done_testing();
