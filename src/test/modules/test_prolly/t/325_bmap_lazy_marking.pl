# Copyright (c) 2026, PostgreSQL Global Development Group

# A snapshot marks no buffer: it raises the generation of sharing, and a
# page settled at an older one is checked again before it next changes.  A
# check that began before a snapshot and ends after it leaves the page
# unsettled, since its answer ("not shared") predates the sharing: here a
# hint's check is held between the storage manager's answer and settling
# the page while a snapshot of its relation is taken, and the page's next
# change must still copy it, so that the snapshot keeps the rows it had.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('bmap_lazy_marking');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE EXTENSION test_prolly;
	SET debug_block_mapped_heap = on;
	CREATE TABLE u (a int);
	INSERT INTO u VALUES (1);
});
my $u = sql(q{SELECT pg_relation_filenode('u')});

for my $round (1 .. 2)
{
	# (rows of one page, written out, their hints not set yet: the page its
	# own and settled)
	sql(qq{SET debug_block_mapped_heap = on;
		CREATE TABLE t$round (a int, b int) WITH (fillfactor = 10);
		INSERT INTO t$round SELECT g, $round FROM generate_series(1, 10) g});
	sql(q{CHECKPOINT});
	my $t = sql(qq{SELECT pg_relation_filenode('t$round')});

	# (another relation's snapshot: every page is to be checked again)
	sql(qq{SELECT test_bmap_snapshot_take($u)});

	# a scan setting the page's hints asks whether it is shared: not yet
	my $scan = $node->background_psql('postgres');
	$scan->query_safe(q{
		SELECT injection_points_set_local();
		SELECT injection_points_attach('buffer-smgr-checked', 'wait');
	});
	$scan->query_until(qr/scanning/,
		"\\echo scanning\nSELECT count(*) FROM t$round;\n");
	$node->wait_for_event('client backend', 'buffer-smgr-checked');

	# a snapshot of the page's relation while the check is held
	my $snap = sql(qq{SELECT test_bmap_snapshot_take($t)});
	sql(q{SELECT injection_points_wakeup('buffer-smgr-checked')});
	$scan->quit;
	my $rows = sql(qq{SELECT test_bmap_rows_digest($t)});

	# the page changes: copied, so the snapshot keeps the rows it had
	# (and written out, to whatever block it now has)
	sql(qq{UPDATE t$round SET b = -1 WHERE a = 1});
	sql(q{CHECKPOINT});
	my $then = sql(qq{SELECT test_bmap_share_snapshot($t, $snap)});
	is(sql(qq{SELECT test_bmap_rows_digest($then)}), $rows,
		"round $round: the snapshot keeps its rows");
	isnt(sql(qq{SELECT test_bmap_rows_digest($t)}), $rows,
		"round $round: the relation changed");
	sql(qq{SELECT test_bmap_unlink($then)});
	sql(qq{SELECT test_bmap_snapshot_release($t, $snap)});
}

done_testing();
