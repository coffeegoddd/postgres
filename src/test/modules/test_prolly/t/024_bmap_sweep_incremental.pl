# Copyright (c) 2026, PostgreSQL Global Development Group

# A sweep counts what the captured roots hold.  It must count only the
# roots made or let go of since the last sweep, so that its cost doesn't
# grow with the roots held: a database keeping many roots (one for each
# commit of a version history) would otherwise pay for all of them at each
# sweep.  The buffers a sweep reads stand for its cost: one after a root
# is captured, with eleven roots held and with two hundred and two.
#
# Counted so, a sweep must still be exact: it frees a block nothing holds
# (one a crash leaked), never one a root or a map holds, whether the roots
# were counted by earlier sweeps, let go of since, or left by a crash
# between the records of a sweep that was counting them.

use strict;
use warnings FATAL => 'all';
use JSON::PP;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('sweep_incremental');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
max_prepared_transactions = 2
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 2000) g;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});

# The roots' counts kept as the roots counted anew would make them.
sub counts_exact
{
	my ($what) = @_;
	is(sql(q{SELECT whole AND differ = 0 FROM test_bmap_root_counts()}),
		't', "the roots' counts are whole and exact $what");
}

# Capture 'n' roots, each after a change, so that each holds a node of
# its own; sweep; then capture one more and sweep again: the buffers the
# second sweep looked up.
my @roots;

sub sweep_after_roots
{
	my ($n) = @_;

	for my $i (1 .. $n)
	{
		push @roots, sql(qq{UPDATE t SET a = a WHERE a = $i;
			SELECT test_bmap_root_capture($t)});
	}
	sql(q{SELECT * FROM test_bmap_sweep()});
	push @roots, sql(qq{UPDATE t SET a = a WHERE a = 1999;
		SELECT test_bmap_root_capture($t)});
	my $plan = decode_json(sql(q{
		EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, FORMAT JSON)
		SELECT * FROM test_bmap_sweep()}))->[0]{Plan};
	return $plan->{'Shared Hit Blocks'} + $plan->{'Shared Read Blocks'};
}

my $few = sweep_after_roots(10);
my $many = sweep_after_roots(190);
note("a sweep with 11 roots held looked up $few buffers, with 202 $many");
is(sql(q{SELECT held FROM test_bmap_roots()}), '202', 'the roots are held');
cmp_ok($many, '<=', $few + 5,
	'a sweep after one root is captured costs the same with many roots held');
is(sql(q{SELECT counted FROM test_bmap_root_counts()}), '202',
	'every root is counted');
counts_exact('after the sweeps');

# What a root reads, by a copy made of it.  (The copy is dropped with no
# record of a commit to replay the drop: a crash must come after a
# checkpoint that follows it, else recovery makes the copy again, pointing
# to blocks freed since.)
sub root_rows
{
	my ($root) = @_;
	my $copy = sql(qq{SELECT test_bmap_share_root($root)});
	my $rows = sql(qq{SELECT test_bmap_rows_digest($copy)});
	sql(qq{SELECT test_bmap_unlink($copy)});
	return $rows;
}
my $first_rows = root_rows($roots[0]);
my $last_rows = root_rows($roots[-1]);

# Roots counted, let go of: what only they held is freed as the release
# queue drains, not left to a sweep, and the counts lose what they held.
sql(q{SELECT * FROM test_bmap_release_drain()});
my $free_before = sql(q{SELECT test_bmap_free_blocks()});
sql(qq{SELECT test_bmap_root_release($_)}) for @roots[1 .. 100];
cmp_ok(sql(q{SELECT freed FROM test_bmap_release_drain()}), '>', 0,
	'a drain frees what only the roots let go of held');
cmp_ok(sql(q{SELECT test_bmap_free_blocks()}), '>', $free_before,
	'and the store has those blocks free');
counts_exact('after their release');
is(sql(q{SELECT freed || ' ' || recounted FROM test_bmap_sweep()}), '0 0',
	'a sweep then finds nothing to free or recount');
is(root_rows($roots[0]), $first_rows, 'a root kept reads as it did');
is(root_rows($roots[-1]), $last_rows, 'and so does the last');

# The table's newest root let go of with the rest while the table holds
# the pages: its nodes leave what the roots hold, the table's still, a
# part of the drain at a time, and a sweep in the middle counts exactly.
my $own = sql(qq{UPDATE t SET b = 'own' WHERE a = 500;
	SELECT test_bmap_root_capture($t)});
sql(q{SELECT * FROM test_bmap_sweep()});
sql(qq{SELECT test_bmap_root_release($_)}) for ($own, @roots[101 .. $#roots]);
sql(q{SELECT * FROM test_bmap_release_drain(3)});
cmp_ok(sql(q{SELECT test_bmap_release_pending()}), '>', 0,
	'part of the release is left to drain');
is(sql(q{SELECT freed || ' ' || recounted FROM test_bmap_sweep()}), '0 0',
	'a sweep in the middle of it frees and recounts nothing');
counts_exact('in the middle of a release');
sql(q{SELECT * FROM test_bmap_release_drain()});
counts_exact('once it is drained');
sql(q{UPDATE t SET b = 'changed' WHERE a % 7 = 0});
my $t_rows = sql(qq{SELECT test_bmap_rows_digest($t)});
is(sql(q{SELECT freed FROM test_bmap_sweep()}), '0',
	'the table changed, a sweep frees nothing');
is(sql(qq{SELECT test_bmap_rows_digest($t)}), $t_rows,
	'and the table reads its rows');
is(root_rows($roots[0]), $first_rows, 'nor the first root');
@roots = ($roots[0]);

SKIP:
{
	skip 'Injection points not supported by this build', 13
	  if $ENV{enable_injection_points} ne 'yes';

	sql(q{CREATE EXTENSION injection_points});

	# A table whose root a sweep counted, then changed, its pages its own
	# and the root's; its drop, prepared, commits, and a crash comes before
	# its storage is queued: the table's pages are leaked, the root's held.
	sub leak_table
	{
		my ($name) = @_;
		sql(qq{CREATE TABLE $name (a int, b text) WITH (autovacuum_enabled = off);
			INSERT INTO $name SELECT g, repeat('l', 200) FROM generate_series(1, 3000) g});
		my $filenode = sql(qq{SELECT pg_relation_filenode('$name')});
		my $root = sql(qq{SELECT test_bmap_root_capture($filenode)});
		my $rows = root_rows($root);
		sql(q{SELECT * FROM test_bmap_sweep()});
		sql(qq{UPDATE $name SET b = 'changed' WHERE a % 2 = 0});
		sql(qq{BEGIN; DROP TABLE $name; PREPARE TRANSACTION 'p'});
		sql('CHECKPOINT');

		my $s = $node->background_psql('postgres', on_error_stop => 0);
		$s->query_safe(q{SELECT injection_points_set_local();
			SELECT injection_points_attach('bmap-predrop', 'wait')});
		$s->query_until(qr/running/, q{
			\echo running
			COMMIT PREPARED 'p';
		});
		$node->wait_for_event('client backend', 'bmap-predrop');
		$node->stop('immediate');
		eval { $s->quit };
		$node->start;
		is(sql(qq{SELECT to_regclass('$name') IS NULL}), 't',
			"$name: the drop committed");
		return ($root, $rows);
	}

	my ($root, $rows) = leak_table('leaked');
	is(sql(q{SELECT whole FROM test_bmap_root_counts()}), 't',
		"leaked: the roots' counts are whole after the crash");
	cmp_ok(sql(q{SELECT freed FROM test_bmap_sweep()}), '>', 0,
		'leaked: a sweep beginning from the roots counted before frees the leaked blocks');
	is(root_rows($root), $rows, 'leaked: and none the root holds');
	is(sql(q{SELECT freed FROM test_bmap_sweep()}), '0',
		'leaked: a second sweep finds nothing more');
	counts_exact('leaked: after it');

	# Again, with a crash between the records of the sweep that counts the
	# roots: it said the counts were not whole, then wrote some of them.
	($root, $rows) = leak_table('leaked_again');
	sql(qq{UPDATE t SET a = a WHERE a = 3;
		SELECT test_bmap_root_capture($t)});
	sql('CHECKPOINT');
	sql(q{SELECT injection_points_attach('bmap-sweep-root-counts', 'wait')});
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->query_until(qr/sweeping/, q{
		\echo sweeping
		SELECT * FROM test_bmap_sweep();
	});
	$node->wait_for_event('client backend', 'bmap-sweep-root-counts');
	# (its records flushed by a commit that writes after them)
	sql(q{CREATE TABLE wal_flushed (a int)});
	$node->stop('immediate');
	eval { $s->quit };
	$node->start;
	is(sql(q{SELECT whole FROM test_bmap_root_counts()}), 'f',
		"a crash in a sweep's counting leaves the roots' counts not whole");
	cmp_ok(sql(q{SELECT freed FROM test_bmap_sweep()}), '>', 0,
		'the next sweep, counting every root anew, frees the leaked blocks');
	is(root_rows($root), $rows, 'and none the root holds');
	is(root_rows($roots[0]), $first_rows, 'nor any an older root holds');
	counts_exact('after it');
	is(sql(q{SELECT freed FROM test_bmap_sweep()}), '0',
		'a sweep after it finds nothing more');
}

$node->stop;
done_testing();
