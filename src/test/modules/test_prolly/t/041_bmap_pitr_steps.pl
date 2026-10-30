# Copyright (c) 2026, PostgreSQL Global Development Group

# Point-in-time recovery to a target inside each block-map operation that
# logs more than one record: a share, a copy on first write, a drop and
# its release, a truncation of shared storage, and a sweep.  Restored from
# a base backup and archived WAL to the end of each record of the step in
# turn (at most four, spread over it), then promoted, the server starts
# and reads as it did at the target:
# - the source's rows are those before or after the step, and
#   verify_heapam finds nothing wrong;
# - each copy, a table made in the share's transaction, exists only if
#   that transaction committed by the target, and then reads as its
#   source did when shared;
# - a sweep afterwards frees only what nothing holds: everything still
#   reads the same after it.

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

sub rows
{
	my ($node, $table) = @_;
	return 'none'
	  if $node->safe_psql('postgres', qq{SELECT to_regclass('$table') IS NULL}) eq 't';
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

my @tables = ('t', 'c1', 'c2');
my @steps;

# Run a step, noting the WAL it logged and every table's rows before and
# after it.
sub step
{
	my ($name, $do) = @_;
	my %before = map { $_ => rows($primary, $_) } @tables;
	my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
	$do->();
	sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
	my $end = sql(q{SELECT pg_current_wal_insert_lsn()});
	my %after = map { $_ => rows($primary, $_) } @tables;
	push @steps, { name => $name, start => $start, end => $end,
				   before => \%before, after => \%after };
}

step('share', sub { share_as('c1') });
step('copy on first write', sub { sql(q{UPDATE t SET b = 'changed' WHERE a % 10 = 0}) });
step('drop and release', sub {
	sql(q{DROP TABLE c1});
	sql(q{SELECT * FROM test_bmap_release_drain()});
});
share_as('c2');
step('truncation of shared storage', sub {
	sql(q{DELETE FROM t WHERE a > 3000});
	sql(q{VACUUM t});
});
step('sweep', sub { sql(q{SELECT * FROM test_bmap_sweep()}) });
my $last = sql(q{SELECT pg_walfile_name(pg_current_wal_lsn())});
sql(q{SELECT pg_switch_wal()});
$primary->poll_query_until('postgres',
	qq{SELECT last_archived_wal >= '$last' FROM pg_stat_archiver})
  or die 'WAL not archived';

my $n = 0;
for my $s (@steps)
{
	my @ends = split /\n/, sql(qq{
		SELECT end_lsn FROM pg_get_wal_records_info('$s->{start}', '$s->{end}')
		 WHERE resource_manager <> 'LogicalMessage'
		 ORDER BY start_lsn});
	cmp_ok(scalar(@ends), '>', ($s->{name} eq 'sweep' ? 0 : 1),
		"$s->{name}: logs " . scalar(@ends) . ' records');
	my @targets = @ends > 4 ? @ends[ map { int($_ * $#ends / 3) } 0 .. 3 ] : @ends;

	for my $lsn (@targets)
	{
		$n++;
		my $node = PostgreSQL::Test::Cluster->new("pitr_steps_$n");
		$node->init_from_backup($primary, 'base', has_restoring => 1);
		$node->append_conf('postgresql.conf', qq{
recovery_target_lsn = '$lsn'
recovery_target_action = 'promote'
});
		my $what = "$s->{name} to $lsn";
		ok($node->start(fail_ok => 1), "$what: the server starts") or next;
		$node->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
		  or die "$what: not promoted";

		my %got;
		for my $table (@tables)
		{
			$got{$table} = rows($node, $table);
			ok($got{$table} eq $s->{before}{$table} || $got{$table} eq $s->{after}{$table},
				"$what: $table reads as before or after the step");
		}
		is($node->safe_psql('postgres', q{SELECT count(*) FROM verify_heapam('t')}), '0',
			"$what: t is sound");
		$node->safe_psql('postgres', q{SELECT * FROM test_bmap_sweep()});
		is(rows($node, $_), $got{$_}, "$what: a sweep then leaves $_ as it was")
		  for @tables;
		$node->stop('immediate');
	}
}

done_testing();
