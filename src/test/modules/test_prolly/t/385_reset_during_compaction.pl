# Copyright (c) 2026, PostgreSQL Global Development Group

# A page sweep's compaction holds each relation whose blocks it moves, and
# stops its moves once another waits for that hold.  A hard reset queues
# for its locks, as TRUNCATE does: the compaction lets go after the move
# under way, and the reset gives the table back its kept pages (no row
# written).  With lock_timeout, it waits no longer than that, and fails.  A branch's deletion asks the same of a compaction holding the
# branch, and waits for that one hold: asked, the compaction passes over
# the branch's other relations.  A lowering of shared_below, which holds
# each relation for a pass over its map, is waited for the same way.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('reset_during_compaction');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE EXTENSION test_prolly;
	ALTER DATABASE postgres SET prolly.restore_min_changes = 0;
	CREATE SCHEMA kp VERSIONED;
	CREATE TABLE kp.t (a int, b text) WITH (fillfactor = 10);
	INSERT INTO kp.t SELECT g, repeat('t', 300) FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'load');
});

# kp.t's pages, changed in turn with x extended, so that its own pages
# (copied from the kept ones) lie among x's: x's freed, they're moved down
sub interleave
{
	my ($session_sql, $branch) = @_;
	my $checkout = $branch ? "SELECT dolt_checkout('$branch');" : '';
	sql(qq{CREATE TABLE x (a int, b text) WITH (fillfactor = 10)});
	sql(qq{$checkout DO \$\$
		BEGIN
			FOR i IN 0 .. 199 LOOP
				UPDATE kp.t SET b = repeat('u', 300) WHERE a BETWEEN i * 10 + 1 AND i * 10 + 10;
				INSERT INTO x SELECT g, repeat('x', 300) FROM generate_series(i * 10, i * 10 + 9) g;
			END LOOP;
		END \$\$});
	sql(q{CHECKPOINT; DROP TABLE x});
	sql(q{SELECT test_bmap_release_drain()});
}

# kp.t's file number, on the session's branch or on 'branch'
sub filenode
{
	my ($branch) = @_;
	my $checkout = $branch ? "SELECT dolt_checkout('$branch');" : '';
	my @out = split /\n/, sql(qq{$checkout SELECT pg_relation_filenode('kp.t')});
	return $out[-1];
}

# a sweep stopped before a move of kp.t's blocks (on 'branch' if given),
# holding it: the point is told each relation's file number, and stops at
# kp.t's only, whatever the sweep reaches first
sub start_sweep
{
	my ($branch) = @_;
	my $filenode = filenode($branch);
	my $sweep = $node->background_psql('postgres');
	my $pid = $sweep->query_safe(q{SELECT pg_backend_pid()});
	$sweep->query_safe(qq{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-compact-room', 'wait', '$filenode')});
	$sweep->query_until(qr/started/, q{
		\echo started
		SELECT 'moved ' || moved FROM prolly_page_sweep();
		\echo done
	});
	$node->poll_query_until('postgres', qq{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE pid = $pid AND wait_event = 'bmap-compact-room'})
	  or die 'the compaction did not stop';
	return ($sweep, $pid);
}

sub wake_sweep
{
	my ($sweep) = @_;
	sql(q{SELECT injection_points_detach('bmap-compact-room')});
	# (none waits if it moved on and found nothing more to move)
	$node->psql('postgres', q{SELECT injection_points_wakeup('bmap-compact-room')});
	my $out = $sweep->query_until(qr/done/, '');
	$sweep->quit;
	my ($moved) = $out =~ /moved (\d+)/;
	return $moved;
}

sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT coalesce((SELECT value FROM pg_stat_prolly
		WHERE name = '$name'), 0)});
}

sub held_by
{
	my ($pid) = @_;
	return sql(qq{SELECT count(*) FROM pg_locks l JOIN pg_class c ON c.oid = l.relation
		WHERE l.pid = $pid AND l.locktype = 'relation' AND l.granted
		  AND c.relnamespace = 'kp'::regnamespace});
}

# a hard reset while the compaction holds kp.t
interleave();
my ($sweep, $sweeper) = start_sweep();
is(held_by($sweeper), '1', 'the compaction stopped holding a versioned relation');
my $held = sql(qq{SELECT l.relation FROM pg_locks l JOIN pg_class c ON c.oid = l.relation
	WHERE l.pid = $sweeper AND l.locktype = 'relation' AND l.granted
	  AND c.relnamespace = 'kp'::regnamespace});

my $restored = counted('relations_restored');
my $applied = counted('apply_rows');
my $reset = $node->background_psql('postgres');
my $resetter = $reset->query_safe(q{SELECT pg_backend_pid()});
$reset->query_until(qr/started/, q{
	\echo started
	SELECT dolt_reset('--hard');
	SELECT pg_stat_force_next_flush();
	\echo done
});
ok( $node->poll_query_until(
		'postgres', qq{SELECT wait_event_type = 'Lock'
		FROM pg_stat_activity WHERE pid = $resetter}),
	'the reset waits in the lock\'s queue for the compaction to let go');

# one move let through: asked, the compaction lets go of the relation
# before the next (which would stop again)
sql(q{SELECT injection_points_wakeup('bmap-compact-room')});
ok( $node->poll_query_until(
		'postgres', qq{SELECT count(*) = 0 FROM pg_locks
		WHERE pid = $sweeper AND locktype = 'relation' AND relation = $held}),
	'the compaction lets go after the move under way');
wake_sweep($sweep);
$reset->query_until(qr/done/, '');
$reset->quit;
cmp_ok(counted('relations_restored'), '>', $restored,
	'the reset gives back the kept pages');
is(counted('apply_rows'), $applied, 'writing no row');
is(sql(q{SELECT count(*) FROM kp.t WHERE b = repeat('t', 300)}), '2000',
	'kp.t reads as committed');
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'the working set is clean');

# with lock_timeout, a reset waits for a compaction no longer than that:
# it fails, writing no row, as when a writer holds the table
interleave();
($sweep, $sweeper) = start_sweep();
$restored = counted('relations_restored');
$applied = counted('apply_rows');
my ($ret, $out, $err) = $node->psql('postgres',
	q{SET lock_timeout = '200ms'; SELECT dolt_reset('--hard')});
like($err, qr/canceling statement due to lock timeout/,
	'a reset held up past lock_timeout fails');
is(counted('relations_restored'), $restored, 'restoring no pages');
is(counted('apply_rows'), $applied, 'and writing no row');
wake_sweep($sweep);
sql(q{SELECT dolt_reset('--hard')});
is(sql(q{SELECT count(*) FROM kp.t WHERE b = repeat('t', 300)}), '2000',
	'kp.t reads as committed');

# a branch's deletion while the compaction holds the branch (no session on it)
sql(q{SELECT dolt_branch('gone')});
interleave(undef, 'gone');
($sweep, $sweeper) = start_sweep('gone');
cmp_ok(sql(qq{SELECT count(*) FROM pg_locks WHERE pid = $sweeper
	AND locktype = 'object' AND classid = 'pg_branch'::regclass AND granted}),
	'>', 0, 'the compaction holds a branch');
my $deleter = $node->background_psql('postgres');
my $deleterpid = $deleter->query_safe(q{SELECT pg_backend_pid()});
$deleter->query_until(qr/started/, q{
	\echo started
	SELECT 'deleted' WHERE dolt_branch('-D', 'gone') IS NOT NULL;
	\echo done
});
ok( $node->poll_query_until(
		'postgres', qq{SELECT wait_event = 'BmapCompactionLetGo'
		FROM pg_stat_activity WHERE pid = $deleterpid}),
	'the deletion waits for the compaction to let go');

# one move let through, the point still attached: the compaction lets go
# of the branch and passes over its other relations (its change logs, its
# indexes), so the deletion waits for that one hold, not for each
sql(q{SELECT injection_points_wakeup('bmap-compact-room')});
like($deleter->query_until(qr/done/, ''), qr/deleted/,
	'and deletes the branch after that one move');
$deleter->quit;
is(sql(q{SELECT count(*) FROM dolt_branches WHERE name = 'gone'}), '0', 'gone');
wake_sweep($sweep);

# a lowering of shared_below holds each relation too, for a pass over its
# map that doesn't stop part way: a reset waits for that pass (no longer)
interleave();
# (stopped at kp.t only, as start_sweep() stops a compaction)
my $filenode = filenode();
$sweep = $node->background_psql('postgres');
$sweeper = $sweep->query_safe(q{SELECT pg_backend_pid()});
$sweep->query_safe(qq{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-shared-below-held', 'wait', '$filenode')});
$sweep->query_until(qr/started/, q{
	\echo started
	SELECT 'moved ' || moved FROM prolly_page_sweep();
	\echo done
});
$node->poll_query_until('postgres', qq{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE pid = $sweeper AND wait_event = 'bmap-shared-below-held'})
  or die 'the lowering did not stop';
is( sql(qq{SELECT count(*) FROM pg_locks l JOIN pg_class c ON c.oid = l.relation
		WHERE l.pid = $sweeper AND l.locktype = 'relation' AND l.granted
		  AND c.oid = 'kp.t'::regclass}),
	'1', 'the lowering of shared_below stopped holding kp.t');
$restored = counted('relations_restored');
$applied = counted('apply_rows');
$reset = $node->background_psql('postgres');
$resetter = $reset->query_safe(q{SELECT pg_backend_pid()});
$reset->query_until(qr/started/, q{
	\echo started
	SELECT dolt_reset('--hard');
	SELECT pg_stat_force_next_flush();
	\echo done
});
ok( $node->poll_query_until(
		'postgres', qq{SELECT wait_event_type = 'Lock'
		FROM pg_stat_activity WHERE pid = $resetter}),
	'the reset waits for the lowering to let go');
sql(q{SELECT injection_points_detach('bmap-shared-below-held')});
sql(q{SELECT injection_points_wakeup('bmap-shared-below-held')});
$reset->query_until(qr/done/, '');
$reset->quit;
$sweep->query_until(qr/done/, '');
$sweep->quit;
cmp_ok(counted('relations_restored'), '>', $restored,
	'the reset gives back the kept pages');
is(counted('apply_rows'), $applied, 'writing no row');
is(sql(q{SELECT count(*) FROM kp.t WHERE b = repeat('t', 300)}), '2000',
	'kp.t reads as committed');


# lock_timeout bounds each lock's wait, not a statement's: a procedure
# committing between two resets gives the second its own, so it still
# waits for a compaction long after the first one waited
sql(q{CREATE PROCEDURE two_resets() LANGUAGE plpgsql AS $$
	BEGIN
		SET lock_timeout = '2s';
		PERFORM dolt_reset('--hard');
		COMMIT;
		SET lock_timeout = 0;
		PERFORM pg_advisory_lock(42);
		PERFORM pg_advisory_unlock(42);
		PERFORM pg_sleep(2.5);
		SET lock_timeout = '2s';
		PERFORM dolt_reset('--hard');
		COMMIT;
	END $$});
interleave();
($sweep, $sweeper) = start_sweep();
my $gate = $node->background_psql('postgres');
$gate->query_safe(q{SELECT pg_advisory_lock(42)});
my $proc = $node->background_psql('postgres');
my $procpid = $proc->query_safe(q{SELECT pg_backend_pid()});
$proc->query_until(qr/started/, q{
	\echo started
	CALL two_resets();
	SELECT pg_stat_force_next_flush();
	\echo done
});
ok( $node->poll_query_until(
		'postgres', qq{SELECT wait_event = 'relation'
		FROM pg_stat_activity WHERE pid = $procpid}),
	"the procedure's first reset waits for the compaction");
sql(q{SELECT injection_points_wakeup('bmap-compact-room')});
$node->poll_query_until('postgres', qq{SELECT wait_event = 'advisory'
	FROM pg_stat_activity WHERE pid = $procpid})
  or die 'the procedure did not reach its gate';
wake_sweep($sweep);
interleave();
($sweep, $sweeper) = start_sweep();
$restored = counted('relations_restored');
$applied = counted('apply_rows');
$gate->query_safe(q{SELECT pg_advisory_unlock(42)});
$gate->quit;
$node->poll_query_until('postgres', qq{SELECT wait_event = 'relation'
	OR state = 'idle' FROM pg_stat_activity WHERE pid = $procpid})
  or die 'the procedure neither waited nor ended';
is(sql(qq{SELECT wait_event FROM pg_stat_activity WHERE pid = $procpid}),
	'relation',
	'its second reset, past the first one\'s lock_timeout, still waits');
sql(q{SELECT injection_points_wakeup('bmap-compact-room')});
$proc->query_until(qr/done/, '');
$proc->quit;
wake_sweep($sweep);
cmp_ok(counted('relations_restored'), '>', $restored,
	'and gives back the kept pages');
is(counted('apply_rows'), $applied, 'writing no row');
is(sql(q{SELECT count(*) FROM kp.t WHERE b = repeat('t', 300)}), '2000',
	'kp.t reads as committed');

$node->stop;
done_testing();
