# Copyright (c) 2026, PostgreSQL Global Development Group

# Work that sets aside the errors it expects still ends with a cancel, a
# timeout or a termination.  A move that makes a routine with a
# SQL-standard body, or a view, tries each in a subtransaction of its own,
# and tries again later one that uses what isn't made yet: a cancel while
# one runs ends the move, the branch left where it was, rather than being
# retried as such a failure.  The same goes for a VACUUM's release step, whose failure is otherwise a
# warning, and for a page sweep's compaction of a relation, whose failure
# otherwise passes the relation over, and for a table rewrite's
# checkpoint, whose failure is otherwise logged: running out of resources
# fails the ALTER.  A definition failing for its own reason until another
# of the move's statements has run is still set aside and tried again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('cancel_not_set_aside');
$node->init;
$node->append_conf('postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION injection_points; CREATE EXTENSION test_prolly});

# Run $query in a session stopped at injection point $point, and end it
# there as $how says: 'cancel' (pg_cancel_backend), 'timeout' (the
# query's statement_timeout runs out as it waits) or 'terminate'
# (pg_terminate_backend).  What the session printed (stdout, stderr), or
# for a session terminated, the server log since it began.  The
# point is let go of once the cancel is sent, so that work that sets the
# cancel aside and tries again isn't stopped once more.  The session takes
# the cancel as it waits, or, if it holds off interrupts there, at its
# next check for them.
sub cancelled_at
{
	my ($point, $query, $cond, $how) = @_;
	$how //= 'cancel';
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	my $pid = $s->query_safe(q{SELECT pg_backend_pid()});
	my $offset = -s $node->logfile;
	my $arg = defined $cond ? ", '$cond'" : '';
	$s->query_safe(qq{SELECT injection_points_set_local();
		SELECT injection_points_attach('$point', 'wait'$arg)});
	$s->query_safe(q{SET statement_timeout = '2s'}) if $how eq 'timeout';
	$s->{stderr} = '';
	$s->query_until(qr/started/, qq{
		\\echo started
		$query;
		\\echo done
	});
	$node->poll_query_until('postgres', qq{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE pid = $pid AND wait_event = '$point'})
	  or die "the session did not stop at $point";
	if ($how eq 'timeout')
	{
		# (ended by its timeout as it waits)
		$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_stat_activity
			WHERE pid = $pid AND wait_event = '$point'})
		  or die "the session did not time out at $point";
	}
	else
	{
		sql($how eq 'terminate' ? qq{SELECT pg_terminate_backend($pid)}
			: qq{SELECT pg_cancel_backend($pid)});
	}
	# (a point attached for the session goes with it, if it is terminated;
	# none waits once the cancel is taken)
	$node->psql('postgres', qq{SELECT injection_points_detach('$point')});
	$node->psql('postgres', qq{SELECT injection_points_wakeup('$point')});
	if ($how eq 'terminate')
	{
		$node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM pg_stat_activity
			WHERE pid = $pid})
		  or die "the session was not terminated";
		# (what the session's server log says: its psql is gone)
		eval { $s->quit };
		return ('', slurp_file($node->logfile, $offset));
	}
	my $out = $s->query_until(qr/done/, '');
	my $err = $s->{stderr};
	$s->quit;
	return ($out, $err);
}

# -- a move making a routine with a SQL-standard body, then a view
sql(q{CREATE SCHEMA mv VERSIONED;
	CREATE TABLE mv.t (id int PRIMARY KEY);
	INSERT INTO mv.t VALUES (1)});
my $c1 = (split /\n/, sql(q{SELECT (dolt_commit('-Am', 'table'))[1]}))[-1];
sql(q{CREATE FUNCTION mv.f() RETURNS int LANGUAGE sql
	BEGIN ATOMIC SELECT count(*)::int FROM mv.t; END});
my $c2 = (split /\n/, sql(q{SELECT (dolt_commit('-Am', 'routine'))[1]}))[-1];
# (two views: one cancelled, the other made, the first would be retried)
sql(q{CREATE VIEW mv.v AS SELECT id FROM mv.t;
	CREATE VIEW mv.w AS SELECT id FROM mv.t});
my $c3 = (split /\n/, sql(q{SELECT (dolt_commit('-Am', 'view'))[1]}))[-1];

sql(qq{SELECT dolt_reset('--hard', '$c1')});
my ($out, $err) = cancelled_at('prolly-move-try-statement',
	qq{SELECT dolt_reset('--hard', '$c2')});
like($err, qr/ERROR:  canceling statement due to user request/,
	'a cancel while a move makes a routine ends the move');
is(sql(q{SELECT dolt_hashof('HEAD')}), $c1, 'the branch is where it was');
is(sql(q{SELECT count(*) FROM pg_proc WHERE proname = 'f'
	AND pronamespace = 'mv'::regnamespace}), '0', 'without the routine');

sql(qq{SELECT dolt_reset('--hard', '$c2')});
($out, $err) = cancelled_at('prolly-move-try-statement',
	qq{SELECT dolt_reset('--hard', '$c3')});
like($err, qr/ERROR:  canceling statement due to user request/,
	'a cancel while a move makes views ends the move');
is(sql(q{SELECT dolt_hashof('HEAD')}), $c2, 'the branch is where it was');
is(sql(q{SELECT to_regclass('mv.v') IS NULL AND to_regclass('mv.w') IS NULL}),
	't', 'without the views');

# a statement_timeout running out, or the session terminated, ends the
# move too
sql(qq{SELECT dolt_reset('--hard', '$c1')});
($out, $err) = cancelled_at('prolly-move-try-statement',
	qq{SELECT dolt_reset('--hard', '$c2')}, undef, 'timeout');
like($err, qr/ERROR:  canceling statement due to statement timeout/,
	'a statement timeout while a move makes a routine ends the move');
is(sql(q{SELECT dolt_hashof('HEAD')}), $c1, 'the branch is where it was');
($out, $err) = cancelled_at('prolly-move-try-statement',
	qq{SELECT dolt_reset('--hard', '$c2')}, undef, 'terminate');
like($err, qr/FATAL:  terminating connection due to administrator command/,
	'a session terminated while its move makes a routine ends');
is(sql(q{SELECT dolt_hashof('HEAD')}), $c1, 'the branch is where it was');
is(sql(q{SELECT count(*) FROM pg_proc WHERE proname = 'f'
	AND pronamespace = 'mv'::regnamespace}), '0', 'without the routine');

# uncancelled, the moves go through
sql(qq{SELECT dolt_reset('--hard', '$c1')});
sql(qq{SELECT dolt_reset('--hard', '$c3')});
is(sql(q{SELECT mv.f(), (SELECT count(*) FROM mv.v), (SELECT count(*) FROM mv.w)}),
	'1|1|1', 'the moves make the routine and the views');

# A definition that fails for its own reason until another statement of
# the move has run is still tried again: a routine grouping by a primary
# key the move adds selects a column only once the key is there, and fails
# with a grouping error (42803) before.
sql(q{CREATE SCHEMA m VERSIONED;
	CREATE TABLE m.t (id int NOT NULL, a int);
	INSERT INTO m.t VALUES (1, 10)});
my $ma = (split /\n/, sql(q{SELECT (dolt_commit('-Am', 'ma'))[1]}))[-1];
sql(q{ALTER TABLE m.t ADD PRIMARY KEY (id);
	CREATE FUNCTION m.f() RETURNS SETOF int LANGUAGE sql
		BEGIN ATOMIC SELECT a FROM m.t GROUP BY id; END});
my $mb = (split /\n/, sql(q{SELECT (dolt_commit('-Am', 'mb'))[1]}))[-1];
# (back without the routine first: a reset doesn't yet drop a routine
# before the constraint it depends on)
sql(q{DROP FUNCTION m.f()});
sql(qq{SELECT dolt_reset('--hard', '$ma')});
my ($rc, $o, $e) = $node->psql('postgres', qq{SELECT dolt_reset('--hard', '$mb')});
is($rc, 0, 'a move making a routine that relies on a key it adds succeeds');
is(sql(q{SELECT dolt_hashof('HEAD') = '} . $mb . q{', (SELECT count(*) FROM m.f())}),
	't|1', 'the branch is at the commit, with the routine');

# -- a VACUUM's release step
sql(q{CREATE TABLE small (a int) WITH (autovacuum_enabled = off);
	INSERT INTO small VALUES (1), (2);
	CREATE TABLE big (a int, b int8) WITH (fillfactor = 10);
	INSERT INTO big SELECT g, g FROM generate_series(1, 20000) g});
sql('DROP TABLE big');
cmp_ok(sql(q{SELECT test_bmap_release_pending()}), '>', 0,
	'a dropped table is queued');
# (the step waits with the queue's pages locked: the cancel is taken at
# the cost-based delay's interrupt check after it)
($out, $err) = cancelled_at('bmap-release-step', 'VACUUM small');
like($err, qr/ERROR:  canceling statement due to user request/,
	'a cancel in a VACUUM\'s release step ends the VACUUM');
unlike($err, qr/could not let go of dropped storage/, 'not a warning');

# -- a page sweep's compaction of a relation
sql(q{CREATE TABLE kp (a int, b text) WITH (fillfactor = 10);
	INSERT INTO kp SELECT g, repeat('t', 300) FROM generate_series(1, 2000) g;
	CREATE TABLE x (a int, b text) WITH (fillfactor = 10)});
sql(q{DO $$
	BEGIN
		FOR i IN 0 .. 199 LOOP
			UPDATE kp SET b = repeat('u', 300) WHERE a BETWEEN i * 10 + 1 AND i * 10 + 10;
			INSERT INTO x SELECT g, repeat('x', 300) FROM generate_series(i * 10, i * 10 + 9) g;
		END LOOP;
	END $$});
sql(q{CHECKPOINT; DROP TABLE x});
sql(q{SELECT test_bmap_release_drain()});
my $filenode = sql(q{SELECT pg_relation_filenode('kp')});
($out, $err) = cancelled_at('bmap-compact-room',
	q{SELECT 'moved ' || moved FROM prolly_page_sweep()}, $filenode);
like($err, qr/ERROR:  canceling statement due to user request/,
	'a cancel in a page sweep\'s compaction ends the sweep');
unlike($out, qr/moved/, 'which returns nothing');
is(sql(q{SELECT count(*) FROM kp}), '2000', 'the table reads as it did');

# -- a table rewrite's checkpoint: a fold that runs out of temporary file
# space (53400) fails the ALTER, rather than being logged as a skipped
# checkpoint
sql(q{CREATE SCHEMA tl VERSIONED;
	CREATE TABLE tl.t (id int PRIMARY KEY, v text);
	CREATE TABLE tl.big (id int PRIMARY KEY, v text);
	INSERT INTO tl.t SELECT g, 'v' FROM generate_series(1, 100) g;
	INSERT INTO tl.big SELECT g, md5(g::text) FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'base')});
sql(q{UPDATE tl.big SET v = 'changed' WHERE id % 2 = 0});
my $offset = -s $node->logfile;
($rc, $o, $e) = $node->psql('postgres', q{
	SET maintenance_work_mem = '64kB';
	SET temp_file_limit = '64kB';
	ALTER TABLE tl.t ALTER COLUMN id TYPE bigint;
});
like($e, qr/ERROR:  temporary file size exceeds "temp_file_limit"/,
	'a rewrite whose checkpoint runs out of temporary file space fails');
unlike(slurp_file($node->logfile, $offset), qr/not checkpointed/,
	'not logged as a skipped checkpoint');
is(sql(q{SELECT format_type(atttypid, atttypmod) FROM pg_attribute
	WHERE attrelid = 'tl.t'::regclass AND attname = 'id'}), 'integer',
	'the column is as it was');

$node->stop;
done_testing();
