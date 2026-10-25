# Copyright (c) 2026, PostgreSQL Global Development Group

# Freeing a dropped relation's blocks runs at the end of a transaction,
# where it must not fail: an error in it is reported as a warning, the
# blocks left for a sweep.  No abort follows to let go of what the error
# left held, so the drop lets go of it itself: an LWLock taken since it
# began (here by an injection point's callback, which raises the error
# holding one), and the map's buffers' content locks, which a later
# checkpoint and sweep take again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('drop_blocks_error_locks');
$node->init;
$node->append_conf('postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION injection_points; CREATE EXTENSION test_prolly});

# a table made and rolled back: its blocks are freed as the abort ends,
# the queuing failing part way with the map's header locked
sql(q{SELECT injection_points_attach('bmap-drop-changes-queued',
	'test_prolly', 'test_error_holding_lwlock', NULL)});
my ($ret, $out, $err) = $node->psql('postgres', q{
	BEGIN;
	CREATE TABLE gone (a int, b text);
	INSERT INTO gone SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g;
	ROLLBACK;
	SELECT 'held ' || test_lwlock_left_held();
}, on_error_stop => 0);
like($err, qr/WARNING:  could not free the blocks of relation .*: error holding an LWLock/,
	'an error freeing the blocks is a warning');
like($out, qr/held f/, 'the LWLock taken since the drop began is let go of');
sql(q{SELECT injection_points_detach('bmap-drop-changes-queued')});

# the map's buffers aren't left locked: a checkpoint writes them and a
# sweep reads the map
sql('CHECKPOINT');
($ret, $out, $err) = $node->psql('postgres', q{SELECT freed >= 0 FROM prolly_page_sweep()});
is($ret, 0, 'a sweep reads the map') or diag($err);
sql(q{CREATE TABLE kept (a int); INSERT INTO kept VALUES (1)});
is(sql(q{SELECT count(*) FROM kept}), '1', 'and the server goes on');

$node->stop;
done_testing();
