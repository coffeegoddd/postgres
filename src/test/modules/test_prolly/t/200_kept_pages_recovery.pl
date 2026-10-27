# Copyright (c) 2026, PostgreSQL Global Development Group

# A hard reset that gives a branch's tables the pages kept as of its head
# (new storage made of each relation's snapshot) is WAL-logged as any
# storage swap is: a standby replays it and reads the head's rows, a crash
# part way through leaves the working set as it was (the new storage the
# reset was making is gone with its transaction), and a crash after it
# commits recovers the head's rows.  A reset after the crash gives the
# pages back again; VACUUM and a commit after it work as after any reset.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('kept_pages_recovery');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
shared_buffers = 16MB
checkpoint_timeout = 1h
wal_keep_size = '1GB'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points; CREATE EXTENSION amcheck');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('kept_pages_recovery_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub rows
{
	my ($n) = @_;
	return $n->safe_psql('postgres',
		q{SET enable_seqscan = off;
		  SELECT md5(string_agg(id || ':' || v || ':' || md5(w), ',' ORDER BY id))
		  FROM kr.t WHERE id > 0});
}

sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}

sub standby_agrees
{
	my ($what) = @_;
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
	is(rows($standby), rows($node), "$what: the standby reads the rows as the primary does");
}

sql(q{CREATE SCHEMA kr VERSIONED;
	CREATE TABLE kr.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL);
	CREATE INDEX t_v ON kr.t (v);
	INSERT INTO kr.t SELECT g, g % 10, repeat(md5(g::text), 1 + g % 300)
	FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'load')});
my $head = rows($node);
standby_agrees('the load');

# dirty, then reset: the pages given back, replayed by the standby
sql(q{UPDATE kr.t SET v = v + 100 WHERE id % 7 = 0;
	DELETE FROM kr.t WHERE id % 11 = 0;
	UPDATE kr.t SET w = 'short' WHERE id % 13 = 0});
isnt(rows($node), $head, 'the working set changed');
my $restored = counted('relations_restored');
my $applied = counted('apply_rows');
sql(q{SELECT dolt_reset('--hard')});
cmp_ok(counted('relations_restored'), '>', $restored, 'the reset gave the pages back');
is(counted('apply_rows'), $applied, 'and wrote no row');
is(rows($node), $head, 'the head\'s rows');
standby_agrees('a reset');

# a crash part way through a reset, as it gives a table storage made of
# a root: the working set is as it was
sql(q{UPDATE kr.t SET v = -1 WHERE id <= 500});
my $dirty = rows($node);
sql('CHECKPOINT');
my $w = $node->background_psql('postgres');
$w->query_safe(q{SELECT injection_points_set_local()});
$w->query_safe(q{SELECT injection_points_attach('bmap-root-shared', 'wait')});
$w->query_until(qr/resetting/, q{\echo resetting
SELECT dolt_reset('--hard');
});
$node->wait_for_event('client backend', 'bmap-root-shared');
sql('CREATE TABLE IF NOT EXISTS flush_wal (); DROP TABLE flush_wal');
$node->stop('immediate');
$w->{run}->finish;
$node->start;
is(rows($node), $dirty, 'a crash part way through a reset leaves the working set');
ok(sql(q{SELECT EXISTS (SELECT FROM dolt_status)}) eq 't', 'still dirty');

# reset again after the crash: given back, then a crash after its commit
$restored = counted('relations_restored');
sql(q{SELECT dolt_reset('--hard')});
cmp_ok(counted('relations_restored'), '>', $restored, 'a reset after the crash gives the pages back');
is(rows($node), $head, 'the head\'s rows');
$node->stop('immediate');
$node->start;
is(rows($node), $head, 'a crash after the reset commits recovers the head\'s rows');
ok(sql(q{SELECT NOT EXISTS (SELECT FROM dolt_status)}) eq 't', 'clean');
standby_agrees('a reset after the crash');

# VACUUM and a commit after it, then a reset to that commit
sql(q{UPDATE kr.t SET v = 7 WHERE id <= 10; VACUUM kr.t;
	SELECT dolt_commit('-am', 'after')});
my $after = rows($node);
sql(q{DELETE FROM kr.t WHERE id <= 1000});
sql(q{SELECT dolt_reset('--hard')});
is(rows($node), $after, 'a reset to the commit made after');
is(sql(q{SELECT bool_and(bt_index_check(indexrelid, true) IS NOT NULL)
	FROM pg_index WHERE indrelid = 'kr.t'::regclass}), 't', 'its indexes valid');
standby_agrees('a reset to a later commit');

# the crashes may have left blocks held by nothing (a dropped storage's,
# freed after its commit), which a sweep frees; then nothing is
sql('CHECKPOINT');
sql(q{SELECT freed FROM prolly_page_sweep()});
is(sql(q{SELECT freed FROM prolly_page_sweep()}), '0', 'a sweep finds nothing held by nothing');

$standby->stop;
$node->stop;
done_testing();
