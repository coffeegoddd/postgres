# Copyright (c) 2026, PostgreSQL Global Development Group

# Sessions on different branches don't wait for each other: a versioned
# relation's copies are locked apart (relation, tuple and predicate locks,
# btree vacuum cycles), on a standby too, while an ordinary table's locks
# are shared by every branch.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('branch_locks');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
autovacuum = off
max_prepared_transactions = 5
});
$primary->start;

sub sql { return $primary->safe_psql($_[1] // 'postgres', $_[0]); }

sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int);
	INSERT INTO app.t SELECT g, g FROM generate_series(1, 1000) g;
	CREATE TABLE plain (id int);
	SELECT dolt_branch('b1');
	SELECT dolt_branch('b2');
});

# whether a statement on a branch gets its lock within a second
sub gets_lock
{
	my ($branch, $query) = @_;
	my ($ret, $out, $err) = $primary->psql("postgres/$branch",
		"SET lock_timeout = '1s'; $query");
	return $ret == 0 ? 'yes' : ($err =~ /lock timeout/ ? 'waits' : "error: $err");
}

# Relation locks.
my $holder = $primary->background_psql('postgres/b1');
$holder->query_safe('BEGIN; LOCK TABLE app.t IN ACCESS EXCLUSIVE MODE; LOCK TABLE plain');
is(gets_lock('b2', 'SELECT count(*) FROM app.t'), 'yes',
	'another branch\'s copy isn\'t locked by it');
is(gets_lock('b1', 'SELECT count(*) FROM app.t'), 'waits', 'the same branch\'s is');
is(gets_lock('b2', 'SELECT count(*) FROM plain'), 'waits', 'an ordinary table is every branch\'s');
$holder->query_safe('COMMIT');

# Tuple locks: a session waiting for a row on one branch holds the row's
# tuple lock; a session on another branch waiting for its own copy of the
# row doesn't queue behind it.
my $a1 = $primary->background_psql('postgres/b1');
my $c1 = $primary->background_psql('postgres/b1', on_error_stop => 0);
my $d2 = $primary->background_psql('postgres/b2');
my $e2 = $primary->background_psql('postgres/b2', on_error_stop => 0);
$a1->query_safe('BEGIN; SELECT * FROM app.t WHERE id = 1 FOR UPDATE');
$c1->query_until(qr/start/, "\\echo start\nUPDATE app.t SET v = -1 WHERE id = 1;\n");
$d2->query_safe('BEGIN; SELECT * FROM app.t WHERE id = 1 FOR UPDATE');
$e2->query_until(qr/start/,
	"\\echo start\nSET lock_timeout = '5s';\nUPDATE app.t SET v = -2 WHERE id = 1;\n");
$primary->poll_query_until('postgres',
	q{SELECT count(*) = 2 FROM pg_stat_activity
	  WHERE wait_event_type = 'Lock' AND query LIKE 'UPDATE app.t%'})
  or die 'the updates did not wait';
$d2->query_safe('COMMIT');
$e2->query('SELECT 1');
is($e2->{stderr}, '', 'a row\'s waiter on another branch doesn\'t hold it up');
is(sql('SELECT v FROM app.t WHERE id = 1', 'postgres/b2'), '-2', 'its update went in');
$a1->query_safe('COMMIT');
$c1->query('SELECT 1');
is(sql('SELECT v FROM app.t WHERE id = 1', 'postgres/b1'), '-1', 'and the first branch\'s');
$_->quit for ($a1, $c1, $d2, $e2);

# Predicate locks: write skew across two branches is no conflict.
my $s1 = $primary->background_psql('postgres/b1', on_error_stop => 0);
my $s2 = $primary->background_psql('postgres/b2', on_error_stop => 0);
for my $s ($s1, $s2)
{
	$s->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT sum(v) FROM app.t');
}
$s1->query_safe('INSERT INTO app.t VALUES (2001, 1)');
$s2->query_safe('INSERT INTO app.t VALUES (2001, 2)');
$s1->query('COMMIT');
$s2->query('COMMIT');
is($s1->{stderr} . $s2->{stderr}, '', 'serializable transactions on two branches both commit');
$_->quit for ($s1, $s2);

# An index whose relcache entry is kept across a checkout (it has new
# storage in the transaction) takes the new branch's predicate locks:
# write skew on one branch is caught, whichever way its sessions got there.
my $w1 = $primary->background_psql('postgres', on_error_stop => 0);
my $w2 = $primary->background_psql('postgres/b1', on_error_stop => 0);
$w1->query_safe(q{BEGIN ISOLATION LEVEL SERIALIZABLE; REINDEX INDEX app.t_pkey;
	SELECT dolt_checkout('b1'); SET LOCAL enable_seqscan = off;
	SELECT * FROM app.t WHERE id > 5000});
$w2->query_safe(q{BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off;
	SELECT * FROM app.t WHERE id > 5000});
$w1->query('INSERT INTO app.t VALUES (5001, 1)');
$w2->query('INSERT INTO app.t VALUES (5002, 2)');
$w1->query('COMMIT');
$w2->query('COMMIT');
like($w1->{stderr} . $w2->{stderr}, qr/could not serialize access/,
	'write skew on one branch fails, an index reloaded across a checkout');
$_->quit for ($w1, $w2);

# A sequence's lock is its copy's on the session's branch: one used before
# a checkout and after it is locked on each branch in turn.
sql(q{CREATE SEQUENCE app.s; SELECT dolt_branch('b3')});
my $seq = $primary->background_psql('postgres', on_error_stop => 0);
$seq->query(q{BEGIN; SELECT nextval('app.s'); SELECT dolt_checkout('b3');
	SELECT nextval('app.s')});
is($seq->{stderr}, '', 'a sequence used on two branches in one transaction');
is(gets_lock('b3', 'ALTER SEQUENCE app.s RESTART'), 'waits',
	'the second branch\'s copy is locked');
$seq->query('COMMIT');
$seq->quit;

# Two branches vacuum the same index at once: each copy's btree vacuum
# cycle is its own.
sql(q{DELETE FROM app.t WHERE id % 2 = 0}, "postgres/$_") for qw(b1 b2);
my $slow = $primary->background_psql('postgres/b1', on_error_stop => 0);
$slow->query_until(qr/start/, "\\echo start\nSET vacuum_cost_delay = 100;\n"
	  . "SET vacuum_cost_limit = 1;\nVACUUM app.t;\n");
$primary->poll_query_until('postgres',
	q{SELECT count(*) > 0 FROM pg_stat_progress_vacuum WHERE phase = 'vacuuming indexes'})
  or die 'the vacuum did not reach the index';
is(gets_lock('b2', 'VACUUM app.t'), 'yes', 'another branch vacuums its copy meanwhile');
sql(q{SELECT pg_cancel_backend(pid) FROM pg_stat_progress_vacuum});
$slow->quit;

# A standby replays an exclusive lock on the copy it was taken on.
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('branch_locks_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'max_standby_streaming_delay = 0');
$standby->start;
my $conflicts = q{SELECT confl_lock FROM pg_stat_database_conflicts WHERE datname = 'postgres'};
my $reader = $standby->background_psql('postgres/b1', on_error_stop => 0);
$reader->query_safe('BEGIN; SELECT count(*) FROM app.t');
sql(q{BEGIN; LOCK TABLE app.t IN ACCESS EXCLUSIVE MODE; SELECT txid_current(); COMMIT},
	'postgres/b2');
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $conflicts), '0',
	'a lock on another branch\'s copy doesn\'t cancel the standby\'s reader');
sql(q{BEGIN; LOCK TABLE app.t IN ACCESS EXCLUSIVE MODE; SELECT txid_current(); COMMIT},
	'postgres/b1');
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $conflicts), '1', 'one on its own branch\'s does');
$reader->quit;

$standby->stop;
$primary->stop;
done_testing();
