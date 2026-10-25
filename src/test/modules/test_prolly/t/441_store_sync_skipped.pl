# Copyright (c) 2026, PostgreSQL Global Development Group

# A share or snapshot of block-mapped storage that holds no block of its
# own names only blocks an earlier share or snapshot wrote out and synced:
# it doesn't sync the database's page store again.  That sync wrote back
# every dirty page the kernel held for the store (a hard reset's discarded
# working set, the dirty working set a first checkout leaves behind), and
# took seconds with a large dirty set (#947).  One that holds blocks of its
# own still syncs, and so does the next one after a flush that was never
# synced (a share whose transaction rolled back), and the first one after
# the server starts.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('store_sync_skipped');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text);
	INSERT INTO app.t SELECT i, 0, repeat('x', 500) FROM generate_series(1, 5000) i;
	SELECT dolt_commit('-Am', 'base');
});

# The page-store syncs one session's statement 'op' ran, and the shares and
# snapshots of it that skipped theirs, after 'setup' in the same session.
sub syncs_of
{
	my ($setup, $op) = @_;
	return sql(qq{
		$setup;
		CREATE TEMP TABLE before AS SELECT * FROM test_prolly_session_stats();
		$op;
		SELECT string_agg(s.name || '=' || (s.value - b.value), ' ' ORDER BY s.name)
		FROM test_prolly_session_stats() s JOIN before b USING (name)
		WHERE s.name IN ('bmap_store_syncs', 'bmap_store_syncs_skipped');
	});
}

my $dirty = q{UPDATE app.t SET v = v + 1 WHERE id % 5 = 0};

# (the commit synced the store, its snapshots holding blocks of their own)
like(syncs_of($dirty, q{SELECT dolt_reset('--hard')}),
	qr/^bmap_store_syncs=0 bmap_store_syncs_skipped=[1-9]/m,
	'a hard reset of a dirty working set skips the sync');
is(sql(q{SELECT count(*), sum(v) FROM app.t}), '5000|0',
	'the hard reset discarded the dirty rows');

sql(q{SELECT dolt_branch('fresh')});
like(syncs_of($dirty, q{SELECT dolt_checkout('fresh')}),
	qr/^bmap_store_syncs=0 bmap_store_syncs_skipped=[1-9]/m,
	'a first checkout of a branch with a dirty source skips the sync');
is(sql(q{SELECT count(*), sum(v) FROM app.t}), '5000|1000',
	'main keeps its dirty rows');
like(sql(q{SELECT dolt_checkout('fresh'); SELECT count(*), sum(v) FROM app.t}),
	qr/^5000\|0$/m, 'the branch has the commit\'s rows');
sql(q{SELECT dolt_reset('--hard')});

# A snapshot holding blocks of its own (pages changed since the last) syncs.
sql(q{SET debug_block_mapped_heap = on;
	CREATE TABLE plain (a int, b text);
	INSERT INTO plain SELECT g, repeat('p', 200) FROM generate_series(1, 2000) g});
my $plain = sql(q{SELECT pg_relation_filenode('plain')});
like(syncs_of(q{UPDATE plain SET a = a + 1 WHERE a % 10 = 0},
		qq{SELECT test_bmap_snapshot_take($plain)}),
	qr/^bmap_store_syncs=1 bmap_store_syncs_skipped=0$/m,
	'a snapshot with blocks of its own syncs the page store');
like(syncs_of(q{SELECT 1}, qq{SELECT test_bmap_snapshot_take($plain)}),
	qr/^bmap_store_syncs=0 bmap_store_syncs_skipped=1$/m,
	'a snapshot with none skips it');

# A share whose transaction rolls back wrote out its blocks without syncing
# them: the next share or snapshot with none of its own syncs after all.
sql(qq{UPDATE plain SET a = a + 1 WHERE a % 10 = 1;
	BEGIN; SELECT test_bmap_share($plain, true); ROLLBACK});
like(syncs_of($dirty, q{SELECT dolt_reset('--hard')}),
	qr/^bmap_store_syncs=1 /m,
	'after a share that was never synced, a hard reset syncs the page store');
like(syncs_of($dirty, q{SELECT dolt_reset('--hard')}),
	qr/^bmap_store_syncs=0 bmap_store_syncs_skipped=[1-9]/m,
	'and the next skips it again');

SKIP:
{
	skip 'injection points not supported by this build', 7
	  unless $ENV{enable_injection_points} eq 'yes';
	sql(q{CREATE EXTENSION injection_points});

	# A share of blocks of its own in flight in another session (its counts
	# raised, its blocks not yet written): a hard reset syncs.  Then the
	# share written, its sync not yet run (its commit waiting): one syncs
	# again.  Once the share's sync ran, the next skips.
	sql(q{UPDATE plain SET a = a + 1 WHERE a % 10 = 2});
	my $b = $node->background_psql('postgres', on_error_stop => 0);
	$b->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-share-map-page', 'wait');
		SELECT injection_points_attach('bmap-sync-shares', 'wait')});
	$b->query_until(qr/started/, qq{\\echo started
		BEGIN;
		SELECT test_bmap_share($plain, true);
	});
	$node->wait_for_event('client backend', 'bmap-share-map-page');
	like(syncs_of($dirty, q{SELECT dolt_reset('--hard')}),
		qr/^bmap_store_syncs=1 /m,
		'a share in flight in another session: a hard reset syncs');
	sql(q{SELECT injection_points_wakeup('bmap-share-map-page')});
	$b->query_until(qr/committing/, q{\echo committing
		COMMIT;
	});
	$node->wait_for_event('client backend', 'bmap-sync-shares');
	like(syncs_of($dirty, q{SELECT dolt_reset('--hard')}),
		qr/^bmap_store_syncs=1 /m,
		'a share written, its sync not yet run: a hard reset syncs');
	sql(q{SELECT injection_points_wakeup('bmap-sync-shares')});
	$b->quit;
	like(syncs_of($dirty, q{SELECT dolt_reset('--hard')}),
		qr/^bmap_store_syncs=0 bmap_store_syncs_skipped=[1-9]/m,
		'once the share synced, the next skips');

	# A keep failing after a snapshot's slot was taken: that relation's own
	# blocks may not have been written.  Each later hard reset syncs until a
	# checkpoint begun since completes.
	my ($ret, $out, $err) = $node->psql('postgres',
		q{UPDATE app.t SET v = v + 1 WHERE id % 7 = 0;
		SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-snapshot-slot-taken', 'error');
		SELECT dolt_commit('-am', 'fails')});
	like($err, qr/bmap-snapshot-slot-taken/, 'the keep failed after a slot was taken');
	like(syncs_of($dirty, q{SELECT dolt_reset('--hard')}),
		qr/^bmap_store_syncs=1 /m,
		'after a flush given up, a hard reset syncs');
	like(syncs_of($dirty, q{SELECT dolt_reset('--hard')}),
		qr/^bmap_store_syncs=1 /m,
		'and so does the next, the sync writing no buffer');
	sql('CHECKPOINT');
	like(syncs_of($dirty, q{SELECT dolt_reset('--hard')}),
		qr/^bmap_store_syncs=0 bmap_store_syncs_skipped=[1-9]/m,
		'after a checkpoint, the next skips');
}

# And after a restart: the first syncs again.
$node->restart;
like(syncs_of($dirty, q{SELECT dolt_reset('--hard')}),
	qr/^bmap_store_syncs=1 /m,
	'after a restart, the first hard reset syncs the page store');

$node->stop;
done_testing();
