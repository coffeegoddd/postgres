# Copyright (c) 2026, PostgreSQL Global Development Group

# Pages a standby's redo adds to a relation stay cached across promotion.
# Recovery raises no generation of sharing as it replays a snapshot of the
# relation, so a page loaded or extended in recovery is settled at none:
# its first change after promotion is checked, and a page the replayed
# snapshot keeps is copied, not changed in place under the snapshot.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('extended_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
shared_buffers = 64MB
wal_keep_size = 1GB
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
});
$primary->start;
$primary->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('extended_standby');
$standby->init_from_backup($primary, 'b', has_streaming => 1);
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	SET debug_block_mapped_heap = on;
	CREATE TABLE t (a int, b int) WITH (fillfactor = 10);
	INSERT INTO t SELECT g, 0 FROM generate_series(1, 20) g;
});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
$primary->wait_for_replay_catchup($standby);

# pages the standby extends the relation by in redo, then a snapshot
sql(q{INSERT INTO t SELECT g, 0 FROM generate_series(21, 400) g});
my $rows = sql(qq{SELECT test_bmap_rows_digest($rel)});
my $snap = sql(qq{SELECT test_bmap_snapshot_take($rel)});
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($rel)}),
	$rows, 'the standby replayed both');
$primary->stop;

# promoted: the extended pages, still cached, change and are written out
$standby->promote;
$standby->safe_psql('postgres', q{UPDATE t SET b = -1 WHERE a > 300});
$standby->safe_psql('postgres', 'CHECKPOINT');
isnt($standby->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($rel)}),
	$rows, 'the relation changed');

# copied first: the snapshot keeps the rows it had
my $then = $standby->safe_psql('postgres',
	qq{SELECT test_bmap_share_snapshot($rel, $snap)});
is($standby->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($then)}),
	$rows, 'the snapshot keeps its rows');

$standby->stop;
done_testing();
