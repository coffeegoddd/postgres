# Copyright (c) 2026, PostgreSQL Global Development Group

# A shared page zeroed to be initialized is copied to a new block without an
# image; redo zeroes it.  A standby that has the old, shared page in a
# buffer zeroes that buffer too: it reads what the primary reads, and the
# page matches the primary's under wal_consistency_checking.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('remap_zero');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
wal_consistency_checking = 'all'
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_buffercache;
	CREATE SCHEMA cow VERSIONED;
});
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('remap_zero_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Replay reaching the primary's WAL, or the standby stopping on an
# inconsistent page.
sub replayed
{
	my $lsn = $node->lsn('insert');
	foreach (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
	{
		return 0
		  if slurp_file($standby->logfile) =~ /inconsistent page found/;
		return 1
		  if sql(qq{SELECT coalesce(bool_or(replay_lsn >= '$lsn'), false)
			FROM pg_stat_replication}) eq 't';
		usleep(100_000);
	}
	return 0;
}

sql(q{
	CREATE TABLE cow.z (a int);
	INSERT INTO cow.z SELECT g FROM generate_series(1, 300) g;
});
my $z = sql(q{SELECT pg_relation_filenode('cow.z')});
my $zcopy = sql(qq{SELECT test_bmap_share($z)});
my $first = q{SELECT count(*) FROM cow.z WHERE ctid < '(1,0)'};
my $rest = q{SELECT count(*) FROM cow.z WHERE ctid >= '(1,0)'};
my $zrest = sql($rest);
ok(replayed(), 'the standby replays the share');

# the standby has the shared page in a buffer; the primary doesn't
cmp_ok($standby->safe_psql('postgres', $first), '>', 0,
	'the standby reads the first page\'s rows');
sql(q{SELECT pg_buffercache_evict_relation('cow.z')});
sql(qq{SELECT test_bmap_zero_block($z, 0)});
# (WAL flushed by a transaction that logs a change: the helper's commits none)
sql(q{CREATE TABLE flushed ()});
is(sql(qq{SELECT refs FROM test_bmap_map($z) WHERE blkno = 0}), '0',
	'the page went to a new block');
is(sql($first), '0', 'the primary reads the zeroed page as empty');

ok(replayed(), 'the standby replays the copy, its page consistent');
is($standby->safe_psql('postgres', $first), '0',
	'the standby reads the zeroed page as empty, from its buffer');
is($standby->safe_psql('postgres', $rest), $zrest, 'and the rest as it was');
$standby->safe_psql('postgres',
	q{SELECT pg_buffercache_evict_relation('cow.z')});
is($standby->safe_psql('postgres', $first), '0', 'and from its new block');

# what the page gets next replays onto zeroes too
sql(q{INSERT INTO cow.z SELECT -g FROM generate_series(1, 5) g});
ok(replayed(), 'the standby replays the page\'s next change');
is($standby->safe_psql('postgres', q{SELECT count(*) FROM cow.z WHERE a < 0}),
	sql(q{SELECT count(*) FROM cow.z WHERE a < 0}),
	'and has the rows the primary has');

$standby->stop;
$node->stop;
done_testing();
