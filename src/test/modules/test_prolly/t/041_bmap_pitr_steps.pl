# Copyright (c) 2026, PostgreSQL Global Development Group

# Point-in-time recovery to a target inside each block-map operation that
# logs more than one record: a share, a copy on first write, a drop and
# its release, a truncation of shared storage, and a sweep.  Each step is
# restored from a base backup and archived WAL to just before each of its
# block-map, storage and transaction records in turn (the first and last
# of each kind), then promoted.  The server starts and reads as it did at the
# target:
# - every table's rows are those before or after the step;
# - a copy, a table made in the share's transaction, exists only if that
#   transaction committed;
# - verify_heapam finds every table sound;
# - storage made by a transaction that never committed (a share stopped
#   before its commit) is dropped as an orphan, and once the release queue
#   is drained, each block's reference count is its holders less one;
# - a sweep then changes no table's rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('pitr_steps');
$primary->init(has_archiving => 1, allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION amcheck;
	CREATE EXTENSION pg_walinspect;
	CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 6000) g;
});
$primary->backup('base');

sub sql { return $primary->safe_psql('postgres', $_[0]); }

my @tables = ('t', 'c1', 'c2');

sub exists_on
{
	my ($node, $table) = @_;
	return $node->safe_psql('postgres', qq{SELECT to_regclass('$table') IS NOT NULL}) eq 't';
}

sub rows
{
	my ($node, $table) = @_;
	return 'none' unless exists_on($node, $table);
	return $node->safe_psql('postgres',
		qq{SELECT md5(coalesce(string_agg(x::text, ',' ORDER BY a), '')) FROM $table x});
}

# A copy of t as table 'name', in one transaction: its storage shares t's.
sub share_as
{
	my ($name) = @_;
	sql(qq{
		BEGIN;
		CREATE TABLE $name (LIKE t);
		UPDATE pg_class SET
			relfilenode = test_bmap_share(pg_relation_filenode('t'), true),
			relfrozenxid = (SELECT relfrozenxid FROM pg_class WHERE relname = 't'),
			relminmxid = (SELECT relminmxid FROM pg_class WHERE relname = 't')
			WHERE relname = '$name';
		COMMIT;
	});
}

my @steps;

# Run a step, noting the WAL it logged and every table's rows before and
# after it, and that it logged the records named in 'expect'.
sub step
{
	my ($name, $expect, $do) = @_;
	my %before = map { $_ => rows($primary, $_) } @tables;
	my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
	$do->();
	sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
	my $end = sql(q{SELECT pg_current_wal_insert_lsn()});
	my %after = map { $_ => rows($primary, $_) } @tables;
	my %kinds = map { $_ => 1 } split /\n/, sql(qq{
		SELECT DISTINCT resource_manager || '/' || record_type
		  FROM pg_get_wal_records_info('$start', '$end')});
	ok($kinds{$_}, "$name: logs $_") for @$expect;
	push @steps, { name => $name, start => $start, end => $end,
				   before => \%before, after => \%after };
}

step('share', [ 'BlockMap/TREE', 'BlockMap/SHARED', 'Transaction/COMMIT' ],
	sub { share_as('c1') });
step('copy on first write', [ 'BlockMap/REMAP', 'Transaction/COMMIT' ],
	sub { sql(q{UPDATE t SET b = 'changed' WHERE a % 10 = 0}) });
step('drop and release', [ 'Transaction/COMMIT', 'BlockMap/RELEASE' ], sub {
	sql(q{DROP TABLE c1});
	sql(q{SELECT * FROM test_bmap_release_drain()});
});
share_as('c2');
# (deleting by position, so that the table's tail empties and is cut)
my $half = int(sql(q{SELECT pg_relation_size('t') / 8192}) / 2);
step('truncation of shared storage', [ 'Storage/TRUNCATE', 'BlockMap/TRUNCATE' ], sub {
	sql(qq{DELETE FROM t WHERE ctid >= '($half,0)'});
	sql(q{VACUUM t});
});
cmp_ok(sql(q{SELECT pg_relation_size('t') / 8192}), '<=', $half, 'the table was cut');
step('sweep', [], sub { sql(q{SELECT * FROM test_bmap_sweep()}) });
my $last = sql(q{SELECT pg_walfile_name(pg_current_wal_lsn())});
sql(q{SELECT pg_switch_wal()});
$primary->poll_query_until('postgres',
	qq{SELECT last_archived_wal >= '$last' FROM pg_stat_archiver})
  or die 'WAL not archived';

# The holders of each block, of every table's map (main fork and
# visibility map), against its count: a count is its holders less one.
my $counts_q = q{
	WITH m AS (
		SELECT phys, refs FROM test_bmap_map(pg_relation_filenode('%s'), 0)
		UNION ALL
		SELECT phys, refs FROM test_bmap_map(pg_relation_filenode('%s'), 2))
	SELECT phys || ':' || max(refs) || ':' || count(*) FROM m GROUP BY phys};

sub counts_off
{
	my ($node) = @_;
	my (%refs, %holders);
	for my $table (grep { exists_on($node, $_) } @tables)
	{
		for (split /\n/, $node->safe_psql('postgres', sprintf($counts_q, $table, $table)))
		{
			my ($phys, $refs, $n) = split /:/;
			$refs{$phys} = $refs;
			$holders{$phys} += $n;
		}
	}
	return scalar grep { $refs{$_} != $holders{$_} - 1 } keys %refs;
}

my $n = 0;
for my $s (@steps)
{
	# Just before the first and the last record of each kind of block-map,
	# storage and transaction record the step logs, and at its end.
	my @recs = map { [ split /\|/ ] } split /\n/, sql(qq{
		SELECT start_lsn, resource_manager || '/' || record_type
		  FROM pg_get_wal_records_info('$s->{start}', '$s->{end}')
		 WHERE resource_manager IN ('BlockMap', 'Storage', 'Transaction')
		 ORDER BY start_lsn});
	my (%first, %last);
	for my $i (0 .. $#recs)
	{
		$first{ $recs[$i][1] } //= $i;
		$last{ $recs[$i][1] } = $i;
	}
	my %pick = map { $_ => 1 } (values %first, values %last);
	my @targets = map { [ $recs[$_][0], $recs[$_][1], $_ ] } sort { $a <=> $b } keys %pick;
	push @targets, [ $s->{end}, 'the end', scalar @recs ];

	# (the share's own tree record: the last before its SHARED record)
	my $share_tree = -1;
	for my $i (0 .. $#recs)
	{
		last if $recs[$i][1] eq 'BlockMap/SHARED';
		$share_tree = $i if $recs[$i][1] eq 'BlockMap/TREE';
	}

	for my $target (@targets)
	{
		my ($lsn, $before_kind) = @$target;
		$n++;
		my $node = PostgreSQL::Test::Cluster->new("pitr_steps_$n");
		$node->init_from_backup($primary, 'base', has_restoring => 1);
		$node->append_conf('postgresql.conf', qq{
recovery_target_lsn = '$lsn'
recovery_target_inclusive = off
recovery_target_action = 'promote'
});
		my $what = "$s->{name}, before $before_kind at $lsn";
		ok($node->start(fail_ok => 1), "$what: the server starts") or next;
		$node->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
		  or die "$what: not promoted";

		my %got;
		for my $table (@tables)
		{
			$got{$table} = rows($node, $table);
			ok($got{$table} eq $s->{before}{$table} || $got{$table} eq $s->{after}{$table},
				"$what: $table reads as before or after the step");
			is($node->safe_psql('postgres', qq{SELECT count(*) FROM verify_heapam('$table')}),
				'0', "$what: $table is sound")
			  if $got{$table} ne 'none';
		}

		# A share stopped after its tree record and before its commit leaves
		# the copy's storage, holding the source's blocks, with no commit or
		# abort to drop it: storage of a transaction that never committed,
		# which is dropped as an orphan.
		my $orphaned = $s->{name} eq 'share' && $share_tree >= 0 &&
		  $target->[2] > $share_tree && $got{c1} eq 'none';
		my $orphans = $node->safe_psql('postgres', q{SELECT test_bmap_drop_orphans()});
		cmp_ok($orphans, '>=', 1, "$what: the uncommitted share's storage is dropped as an orphan")
		  if $orphaned;
		$node->safe_psql('postgres', q{SELECT * FROM test_bmap_release_drain()});
		is(counts_off($node), 0, "$what: each block's count is its holders less one");
		$node->safe_psql('postgres', q{SELECT * FROM test_bmap_sweep()});
		is(rows($node, $_), $got{$_}, "$what: a sweep then leaves $_ as it was")
		  for @tables;
		$node->stop('immediate');
	}
}

done_testing();
