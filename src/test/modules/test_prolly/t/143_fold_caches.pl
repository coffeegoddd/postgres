# Copyright (c) 2026, PostgreSQL Global Development Group

# Working sets taken from an earlier fold (#482), with other sessions: a
# commit landing between a fold's snapshot and its reading of the
# generation, or after that; another session writing, or a prepared
# transaction, while one folds; a restart, with a prepared transaction
# across it, committed after a reader's snapshot too; and a write the generation missed, which the check of
# debug_prolly_verify_fold_caches catches.  None is taken where a fold now
# would differ.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fold_caches');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
max_prepared_transactions = 5
debug_prolly_verify_fold_caches = on
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t SELECT g, 'x' FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'base');
});

my $changes = q{SELECT count(*) FROM dolt_status};

# folds_reused, as a session's own count, flushed first
sub reused
{
	my ($s) = @_;
	$s->query_safe('SELECT pg_stat_force_next_flush()');
	return $s->query_safe(
		q{SELECT value FROM pg_stat_prolly WHERE name = 'folds_reused'});
}

my $a = $node->background_psql('postgres');
$a->query_safe(q{SELECT injection_points_set_local()});

# A commit landing after a fold's snapshot, before it reads the generation
# (W1), or after that (W2): the fold, by the snapshot, misses the row, and
# isn't published; the next status sees the row.
foreach my $point ('prolly-fold-stamp', 'prolly-fold-stamped')
{
	# (a write that changes nothing: the commit's fold is no longer taken)
	sql(q{UPDATE s.t SET v = v WHERE id = 1});
	$a->query_safe(qq{SELECT injection_points_attach('$point', 'wait')});
	$a->query_until(qr/started/, qq{
		\\echo started
		SELECT count(*) AS changes FROM dolt_status;
		\\echo done
	});
	$node->wait_for_event('client backend', $point);
	sql(q{INSERT INTO s.t VALUES (1000, 'landed')});
	sql(qq{SELECT injection_points_wakeup('$point')});
	my $out = $a->query_until(qr/done/, '');
	$a->query_safe(qq{SELECT injection_points_detach('$point')});
	like($out, qr/^0$/m,
		"$point: the fold by the older snapshot misses the row");
	is(sql($changes), '1', "$point: the next status sees it");
	is($a->query_safe($changes), '1', "$point: the folding session's too");
	sql(q{DELETE FROM s.t WHERE id = 1000});
}

# Another session writing, not yet committed: nothing taken while it
# writes; its commit seen.
my $b = $node->background_psql('postgres');
$a->query_safe($changes);
my $before = reused($a);
$b->query_safe(q{BEGIN; INSERT INTO s.t VALUES (2000, 'writing')});
is($a->query_safe($changes), '0', 'a write in progress is no change');
is($a->query_safe($changes), '0', 'still');
is(reused($a), $before, 'and nothing is taken while it writes');
$b->query_safe('COMMIT');
is($a->query_safe($changes), '1', 'its commit is a change');

# A prepared transaction's write: nothing taken while it is prepared; its
# commit, of which nothing is told, seen.
sql(q{SELECT dolt_commit('-am', 'written')});
$a->query_safe($changes);
$b->query_safe(q{BEGIN; INSERT INTO s.t VALUES (3000, 'prepared');
	PREPARE TRANSACTION 'fc'});
$before = reused($a);
is($a->query_safe($changes), '0', 'a prepared write is no change');
is($a->query_safe($changes), '0', 'still');
is(reused($a), $before, 'and nothing is taken while it is prepared');
sql(q{COMMIT PREPARED 'fc'});
is($a->query_safe($changes), '1', 'its commit is a change');
sql(q{SELECT dolt_commit('-am', 'prepared')});

# A write the generation missed: the check folds again and errors.
$a->query_safe($changes);
sql(q{SELECT injection_points_attach('prolly-branch-write-unnoted', 'notice')});
sql(q{INSERT INTO s.t VALUES (4000, 'unnoted')});
sql(q{SELECT injection_points_detach('prolly-branch-write-unnoted')});
my ($ret, $stdout, $stderr) = $node->psql('postgres', $changes);
like($stderr, qr/taken from an earlier fold differs from a fold now/,
	'a missed write: the check catches the fold taken');
is(sql(qq{SET debug_prolly_verify_fold_caches = off; $changes}), '0',
	'without it, the change is missed');
sql(q{INSERT INTO s.t VALUES (4001, 'noted')});
is(sql($changes), '1', 'a write noted ends it');
sql(q{SELECT dolt_commit('-am', 'noted')});

# A fold in a transaction not yet committed, or rolled back: its tree's
# chunks aren't others' yet, or are gone, so no other session takes it.
sql(q{INSERT INTO s.t VALUES (6000, 'open')});
$a->query_safe(q{BEGIN; SELECT count(*) FROM dolt_status});
is(sql($changes), '1', 'a fold in a transaction in progress isn\'t taken');
sql(q{INSERT INTO s.t VALUES (6001, 'open')});
$a->query_safe(q{SELECT count(*) FROM dolt_status; ROLLBACK});
is(sql($changes), '1', 'nor one rolled back');
sql(q{SELECT dolt_commit('-am', 'open')});

# Writers no longer remembered one by one, the latest of them seen by an
# old snapshot and an earlier one not (it was running): that snapshot's
# fold isn't published, though every writer remembered is seen (aborted).
my $diffs = q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)};
my $p = $node->background_psql('postgres');
my $r = $node->background_psql('postgres');
$p->query_safe(q{BEGIN; INSERT INTO s.t VALUES (7000, 'earlier')});
sql(q{INSERT INTO s.t VALUES (7001, 'later')});
$r->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1});
$p->query_safe('COMMIT');
foreach my $i (1 .. 8)
{
	$p->query_safe(qq{BEGIN; INSERT INTO s.t VALUES (8000 + $i, 'aborted');
		ROLLBACK});
}
is($r->query_safe($diffs), '1', 'the old snapshot sees the later writer only');
$r->query_safe('COMMIT');
is(sql(qq{SET debug_prolly_verify_fold_caches = off; $diffs}), '2',
	'its fold isn\'t taken: both writers\' rows are changes');
$p->quit;
$r->quit;
sql(q{SELECT dolt_commit('-am', 'remembered')});

$a->quit;
$b->quit;

# A restart: nothing is known after it.  A prepared transaction across it
# keeps anything from being taken until it ends.
my $c = $node->background_psql('postgres');
$c->query_safe($changes);
$c->query_safe(q{BEGIN; INSERT INTO s.t VALUES (5000, 'across');
	PREPARE TRANSACTION 'across'});
$c->quit;
$node->restart;
$c = $node->background_psql('postgres');
$before = reused($c);
is($c->query_safe($changes), '0', 'after a restart, clean');
is($c->query_safe($changes), '0', 'still');
is(reused($c), $before, 'nothing taken while the prepared one is');
sql(q{COMMIT PREPARED 'across'});
is($c->query_safe($changes), '1', 'its commit is a change');
sql(q{SELECT dolt_commit('-am', 'across')});
$c->query_safe($changes);
$before = reused($c);
is($c->query_safe($changes), '0', 'committed');
is(reused($c), $before + 1, 'and taken again');
$c->quit;

# A prepared transaction across a restart, committed after a reader's
# snapshot: that reader's fold, which misses its row, isn't the working
# set (nothing was told of the prepared one before it ended).
$c = $node->background_psql('postgres');
$c->query_safe(q{BEGIN; INSERT INTO s.t VALUES (5001, 'across again');
	PREPARE TRANSACTION 'across2'});
$c->quit;
$node->restart;
$r = $node->background_psql('postgres');
$r->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ;
	SELECT count(*) FROM s.t});
sql(q{COMMIT PREPARED 'across2'});
is($r->query_safe($changes), '0', 'the older snapshot misses its row');
$r->query_safe('COMMIT');
$r->quit;
is(sql(qq{SET debug_prolly_verify_fold_caches = off; $changes}), '1',
	'its fold isn\'t taken: the commit is a change');
is(sql($changes), '1', 'and the check agrees');

$node->stop;
done_testing();
