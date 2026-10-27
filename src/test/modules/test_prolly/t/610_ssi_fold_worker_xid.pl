# Copyright (c) 2026, PostgreSQL Global Development Group

# Serializable transactions are cleaned up while the fold worker writes.
# A fold's transaction runs with an xid for as long as the fold takes, and
# every snapshot taken meanwhile holds it as running.  A serializable
# transaction's xmin bounds which committed ones may have overlapped it, so
# had the fold's xid counted, every serializable transaction committed
# during the fold would have been kept, its predicate locks and
# rw-conflicts with it, until the pool of rw-conflicts ran out under a
# busy serializable workload.  The fold worker is passed over by predicate
# locking: its xid isn't counted, and the serializable transactions of a
# chain, each overlapping the next, are cleaned up as they would be with
# no fold running, on prolly tables and heap tables alike.  Anomalies are
# still caught meanwhile: a write skew, and a transaction that reads what
# a committed one wrote after reading what it then writes.  Autovacuum's
# automatic fold is passed over the same way.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('ssi_fold_worker_xid');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = on
prolly_fold_worker_naptime = 50ms
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v int);
	INSERT INTO s.t SELECT g, 0 FROM generate_series(1, 100) g;
	CREATE TABLE s.b (id int PRIMARY KEY, v int);
	INSERT INTO s.b SELECT g, 0 FROM generate_series(1, 3000) g;
	CREATE TABLE s.d (id int PRIMARY KEY, oncall bool);
	INSERT INTO s.d VALUES (1, true), (2, true);
	CREATE TABLE s.tw (v int);
	CREATE TABLE public.h (id int PRIMARY KEY, v int);
	INSERT INTO public.h SELECT g, 0 FROM generate_series(1, 100) g;
	CREATE TABLE public.hw (v int);
	SELECT dolt_commit('-Am', 'base');
});
is(sql(q{SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
		 WHERE c.oid = 's.t'::regclass}), 'prolly', 's.t is a prolly table');
my $log = sql(q{SELECT 'pg_toast.pg_prolly_log_' || 's.t'::regclass::oid});
ok($node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM $log}),
	'the log is empty to begin with');

# A fold held after it trimmed the log, its xid assigned and running.
sql(q{SELECT injection_points_attach('prolly-log-trimmed', 'wait')});
sql(q{UPDATE s.t SET v = 1 WHERE id = 100});
ok( $node->poll_query_until(
		'postgres',
		q{SELECT count(*) = 1 FROM pg_stat_activity
		  WHERE backend_type = 'prolly fold worker'
		    AND wait_event = 'prolly-log-trimmed'
		    AND backend_xid IS NOT NULL}),
	'a fold runs with an xid');
sql(q{SELECT injection_points_detach('prolly-log-trimmed')});

my $a = $node->background_psql('postgres', on_error_stop => 0);
my $b = $node->background_psql('postgres', on_error_stop => 0);

# A chain of serializable transactions, each begun before the one before
# it commits: each reads a row of the table and inserts one into another
# that none reads, without an index (so no transaction of the chain
# conflicts with another).  Of
# those committed, none is kept but what the active one may have
# overlapped.
sub chain
{
	my ($table, $sink) = @_;
	my @s = ($a, $b);
	my $n = 12;

	$s[0]->query_safe(
		qq{BEGIN ISOLATION LEVEL SERIALIZABLE;
		   SELECT v FROM $table WHERE id = 1;
		   INSERT INTO $sink VALUES (2)});
	for my $i (1 .. $n)
	{
		my $next = $s[$i % 2];
		my $prev = $s[($i + 1) % 2];
		my ($r, $w) = (2 * $i + 1, 2 * $i + 2);

		$next->query_safe(
			qq{BEGIN ISOLATION LEVEL SERIALIZABLE;
			   SELECT v FROM $table WHERE id = $r;
			   INSERT INTO $sink VALUES ($w)});
		$prev->query_safe('COMMIT');
	}
	my $kept = sql(q{SELECT count(DISTINCT virtualtransaction) FROM pg_locks
					 WHERE mode = 'SIReadLock'});
	$s[$n % 2]->query_safe('COMMIT');
	return $kept;
}

my $kept_prolly = chain('s.t', 's.tw');
my $kept_heap = chain('public.h', 'public.hw');
cmp_ok($kept_prolly, '<=', 3,
	'a chain on a prolly table keeps no more than the transactions an active one overlapped'
);
cmp_ok($kept_heap, '<=', 3,
	'a chain on a heap table keeps no more than the transactions an active one overlapped'
);

# A serializable transaction importing another's snapshot, the source's
# xmin above the fold's xid, which the snapshot holds as running: the
# importer is registered at no lower an xmin (the global xmin doesn't go
# back), and its reads are tracked.
$a->query_safe(
	q{BEGIN ISOLATION LEVEL SERIALIZABLE;
	  SELECT v FROM s.t WHERE id = 1});
my $exported = $a->query_safe(q{SELECT pg_export_snapshot()});
$b->query_safe(
	qq{BEGIN ISOLATION LEVEL SERIALIZABLE;
	   SET TRANSACTION SNAPSHOT '$exported';
	   SELECT v FROM s.t WHERE id = 2});
is(sql(q{SELECT count(DISTINCT virtualtransaction) FROM pg_locks
		 WHERE mode = 'SIReadLock'}),
	'2', 'the importer of a snapshot takes predicate locks as its source does');
$b->query_safe('COMMIT');
$a->query_safe('COMMIT');
is(sql(q{SELECT count(*) FROM pg_locks WHERE mode = 'SIReadLock'}), '0',
	'both are cleaned up');

# A parallel serializable query (its workers share its transaction).
$a->query_safe(
	q{SET debug_parallel_query = on;
	  BEGIN ISOLATION LEVEL SERIALIZABLE;
	  SELECT count(*) FROM s.t;
	  COMMIT;
	  RESET debug_parallel_query});
is(sql(q{SELECT count(*) FROM pg_locks WHERE mode = 'SIReadLock'}), '0',
	'a parallel serializable query is cleaned up');

sub fails_to_serialize
{
	my ($s, $query, $name) = @_;

	$s->{stderr} = '';
	$s->query($query);
	like($s->{stderr}, qr/could not serialize access/, $name);
	# (a failed COMMIT ended the transaction already)
	$s->query('ROLLBACK');
	$s->{stderr} = '';
}

# A write skew: each reads both rows, and takes the other's off call.
$a->query_safe(
	q{BEGIN ISOLATION LEVEL SERIALIZABLE;
	  SELECT count(*) FROM s.d WHERE oncall});
$b->query_safe(
	q{BEGIN ISOLATION LEVEL SERIALIZABLE;
	  SELECT count(*) FROM s.d WHERE oncall});
$a->query_safe(q{UPDATE s.d SET oncall = false WHERE id = 1});
$b->query_safe(q{UPDATE s.d SET oncall = false WHERE id = 2});
$a->query_safe('COMMIT');
fails_to_serialize($b, 'COMMIT', 'a write skew on a prolly table fails');
is(sql(q{SELECT count(*) FROM s.d WHERE oncall}), '1', 'one stays on call');

# A reads x; B reads y, writes x and commits; A writes y: A then follows B
# (it didn't see B's write) and precedes it (B didn't see A's), so A fails,
# by the predicate lock B took, kept though B committed.
sql(q{UPDATE s.d SET oncall = true});
$a->query_safe(
	q{BEGIN ISOLATION LEVEL SERIALIZABLE;
	  SELECT oncall FROM s.d WHERE id = 1});
$b->query_safe(
	q{BEGIN ISOLATION LEVEL SERIALIZABLE;
	  SELECT oncall FROM s.d WHERE id = 2;
	  UPDATE s.d SET oncall = false WHERE id = 1;
	  COMMIT});
fails_to_serialize(
	$a,
	q{UPDATE s.d SET oncall = false WHERE id = 2; COMMIT},
	'a transaction that would precede and follow a committed one fails');
is(sql(q{SELECT count(*) FROM s.d WHERE oncall}), '1',
	'only the committed one took one off call');

# The fold goes on, and commits.
sql(q{SELECT injection_points_wakeup('prolly-log-trimmed')});
ok( $node->poll_query_until(
		'postgres',
		q{SELECT count(*) = 0 FROM pg_stat_activity
		  WHERE backend_type = 'prolly fold worker'
		    AND wait_event = 'prolly-log-trimmed'}),
	'the fold goes on');

# The fold worker doesn't take serializable transactions from the
# configuration.
$node->append_conf('postgresql.conf',
	"default_transaction_isolation = 'serializable'");
$node->reload;
sql(q{UPDATE s.t SET v = 2 WHERE id = 99});
ok($node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM $log}),
	'the fold worker folds with serializable transactions the default');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_locks WHERE mode = 'SIReadLock'}),
	'0', 'no predicate locks are left');

# Autovacuum's automatic fold, the fold worker off, held after it trimmed
# the log, its xid assigned and running.
sql(q{ALTER SYSTEM SET default_transaction_isolation = 'read committed'});
sql(q{ALTER SYSTEM SET prolly_fold_worker = off});
sql(q{ALTER SYSTEM SET autovacuum_naptime = '1s'});
sql(q{ALTER SYSTEM SET prolly.autofold_threshold = 0.001});
sql(q{SELECT pg_reload_conf()});
ok( $node->poll_query_until(
		'postgres',
		q{SELECT count(*) = 0 FROM pg_stat_activity
		  WHERE backend_type = 'prolly fold worker'}),
	'the fold worker is off');
sql(q{SELECT injection_points_attach('prolly-log-trimmed', 'wait')});
sql(q{UPDATE s.b SET v = 1});
sql(q{ALTER SYSTEM SET autovacuum = on; SELECT pg_reload_conf()});
ok( $node->poll_query_until(
		'postgres',
		q{SELECT count(*) = 1 FROM pg_stat_activity
		  WHERE backend_type = 'autovacuum worker'
		    AND wait_event = 'prolly-log-trimmed'
		    AND backend_xid IS NOT NULL}),
	'an automatic fold runs with an xid');
sql(q{SELECT injection_points_detach('prolly-log-trimmed')});

$kept_prolly = chain('s.t', 's.tw');
$kept_heap = chain('public.h', 'public.hw');
cmp_ok($kept_prolly, '<=', 3,
	'during an automatic fold, a chain on a prolly table keeps no more than the transactions an active one overlapped'
);
cmp_ok($kept_heap, '<=', 3,
	'during an automatic fold, a chain on a heap table keeps no more than the transactions an active one overlapped'
);

sql(q{SELECT injection_points_wakeup('prolly-log-trimmed')});
ok( $node->poll_query_until(
		'postgres',
		q{SELECT count(*) = 0 FROM pg_stat_activity
		  WHERE wait_event = 'prolly-log-trimmed'}),
	'the automatic fold goes on');

$a->quit;
$b->quit;

$node->stop;
done_testing();
