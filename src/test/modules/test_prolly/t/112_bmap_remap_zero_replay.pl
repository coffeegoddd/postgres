# Copyright (c) 2026, PostgreSQL Global Development Group

# A shared page zeroed to be initialized is copied to a new block without an
# image; redo zeroes it.  A standby that has the old, shared page in a
# buffer zeroes that buffer too, once no scan holds a pin on it: it reads
# what the primary reads, and the page matches the primary's under
# wal_consistency_checking.  One that hasn't has zeroes in the new block.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

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
# replay waits for a pin as long as it is held
$standby->append_conf('postgresql.conf', 'max_standby_streaming_delay = -1');
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub ssql { return $standby->safe_psql('postgres', $_[0]); }

# Replay reaching the primary's WAL, the standby's pages consistent.  (WAL
# flushed by a transaction that logs a change: the test helpers commit none.)
my $flushes = 0;

sub replayed
{
	$flushes++;
	sql(qq{CREATE TABLE flushed$flushes ()});
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
	return slurp_file($standby->logfile) !~ /inconsistent page found/;
}

sub on_page
{
	my ($blkno) = @_;
	my $next = $blkno + 1;
	return qq{SELECT count(*) FROM cow.z
		WHERE ctid >= '($blkno,0)' AND ctid < '($next,0)'};
}
my $past = q{SELECT count(*) FROM cow.z WHERE ctid >= '(2,0)'};

sql(q{
	CREATE TABLE cow.z (a int);
	INSERT INTO cow.z SELECT g FROM generate_series(1, 1000) g;
});
my $z = sql(q{SELECT pg_relation_filenode('cow.z')});
my $zcopy = sql(qq{SELECT test_bmap_share($z)});
my $zpast = sql($past);
cmp_ok($zpast, '>', 0, 'the relation has pages past the two zeroed');
ok(replayed(), 'the standby replays the share');

# A standby's scan holds a pin on the shared page, which the primary
# doesn't have in a buffer, and zeroes.
my $scan = $standby->background_psql('postgres');
$scan->query_safe(q{BEGIN});
$scan->query_safe(q{DECLARE c CURSOR FOR SELECT a FROM cow.z});
my $fetched = $scan->query_safe(q{FETCH 1 FROM c});
sql(q{SELECT pg_buffercache_evict_relation('cow.z')});
sql(qq{SELECT test_bmap_zero_block($z, 0)});
is(sql(qq{SELECT refs FROM test_bmap_map($z) WHERE blkno = 0}), '0',
	'the page went to a new block');
is(sql(on_page(0)), '0', 'the primary reads the zeroed page as empty');

# Replay waits for the scan before it zeroes the page under it.
sql(q{CREATE TABLE flushed ()});
ok( $standby->poll_query_until(
		'postgres', q{
	SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'startup' AND wait_event = 'BufferCleanup'}),
	'replay waits for the scan\'s pin');
$fetched .= "\n" . $scan->query_safe(q{FETCH ALL FROM c});
my @rows = split /\n/, $fetched;
my $sum = 0;
$sum += $_ foreach @rows;
is(scalar(@rows) . " $sum", '1000 500500',
	'the scan reads every row, the zeroed page\'s too');
$scan->query_safe(q{COMMIT});
$scan->quit;

ok(replayed(), 'the standby replays the copy, its page consistent');
is(ssql(on_page(0)), '0',
	'the standby reads the zeroed page as empty, from its buffer');
is(ssql($past), $zpast, 'and the rest as it was');
ssql(q{SELECT pg_buffercache_evict_relation('cow.z')});
is(ssql(on_page(0)), '0', 'and from its new block');

# A standby without the shared page in a buffer: redo's buffer is zeroes
# already and stays clean, so the new block's zeroes are its own.  The block
# is one freed, which had another relation's page, on disk.
sql(q{
	CREATE TABLE cow.junk (a int);
	INSERT INTO cow.junk SELECT g FROM generate_series(1, 1000) g;
});
ok(replayed(), 'the standby replays another relation');
ssql(q{SELECT pg_buffercache_evict_relation('cow.junk')});
sql(q{DROP TABLE cow.junk});
my $free = sql(q{SELECT test_bmap_free_blocks()});
cmp_ok($free, '>', 0, 'which frees its blocks');
sql(q{SELECT pg_buffercache_evict_relation('cow.z')});
sql(qq{SELECT test_bmap_zero_block($z, 1)});
is(sql(q{SELECT test_bmap_free_blocks()}), $free - 1,
	'the page went to a freed block');
ok(replayed(), 'the standby replays the second copy');
my $phys = ssql(qq{SELECT phys FROM test_bmap_map($z) WHERE blkno = 1});
is( ssql(
		qq{SELECT test_bmap_read_old($z, 1, $phys) =
			decode(repeat('00', current_setting('block_size')::int), 'hex')}
	),
	't',
	'the standby\'s new block has zeroes, below its buffers');
is(ssql(on_page(1)), '0', 'the standby reads that page as empty');
is(ssql($past), $zpast, 'and the rest as it was');

# what the pages get next replays onto zeroes too
sql(q{INSERT INTO cow.z SELECT -g FROM generate_series(1, 5) g});
ok(replayed(), 'the standby replays the pages\' next change');
is( ssql(q{SELECT count(*) FROM cow.z WHERE a < 0}),
	sql(q{SELECT count(*) FROM cow.z WHERE a < 0}),
	'and has the rows the primary has');

sql(qq{SELECT test_bmap_unlink($zcopy)});
ok(replayed(), 'the standby replays dropping the copy');
is( ssql(q{SELECT count(*) FROM cow.z}),
	sql(q{SELECT count(*) FROM cow.z}),
	'and has the relation\'s rows still');

$standby->stop;
$node->stop;
done_testing();
