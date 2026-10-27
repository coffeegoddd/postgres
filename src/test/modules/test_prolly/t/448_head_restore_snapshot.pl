# Copyright (c) 2026, PostgreSQL Global Development Group

# A hard reset to a commit whose kept pages are gone gives a table the
# pages kept as of the head, discarding the working set in O(1), whatever
# other snapshots there are.  A restore gives the table new storage, which
# alone isn't MVCC-safe: a snapshot taken before the reset commits would
# read the head's rows, neither its own nor the commit's.  So the storage
# replaced is kept, and a transaction whose snapshot can't move and is
# older than the reset (a REPEATABLE READ transaction here, which hasn't
# read the table, so holds no lock the restore would wait for) goes on
# reading it: what it saw.  It can't change it (a serialization failure,
# as for a row changed since its snapshot).  The storage is dropped by the
# first keep once no snapshot is older than the reset.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('head_restore_snapshot');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
# (a window of one commit: only the head keeps its root)
sql(q{ALTER DATABASE postgres SET prolly.history_min_commits = 1});
# (the newest commits only: a window that holds no other)
sql(q{ALTER DATABASE postgres SET prolly.history_window = '1 microsecond'});

sql(q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA q VERSIONED;
	CREATE TABLE q.t (id int PRIMARY KEY, v int);
	INSERT INTO q.t SELECT g, 0 FROM generate_series(1, 1000) g;
	CREATE TABLE q.u (id int PRIMARY KEY)});
my $a = sql(q{SELECT (dolt_commit('-Am', 'a'))[1]});
sql(q{UPDATE q.t SET v = 1 WHERE id <= 100; SELECT dolt_commit('-am', 'b')});
sql(q{UPDATE q.t SET v = 2 WHERE id <= 100; SELECT dolt_commit('-am', 'c')});
my $c = sql(q{SELECT dolt_hashof('HEAD')});
my $counts = q{SELECT pg_stat_force_next_flush();
	SELECT value FROM pg_stat_prolly WHERE name = 'relations_restored'};

# (back and forth, so a is no longer kept and c, the head, is: only the
# head is kept here, as a restore carries the other kept commits along)
sub cycle
{
	sql("SELECT dolt_reset('--hard', '$c')");
	sql("SELECT dolt_reset('--hard', '$a')");
	sql("SELECT dolt_reset('--hard', '$c')");
}
cycle();
sql(q{UPDATE q.t SET v = 5});
is(sql('SELECT sum(v) FROM q.t'), '5000', 'the working set changed');

my $rr = $node->background_psql('postgres', on_error_stop => 0);
# (its snapshot taken reading another table: no lock held on q.t, which
# the restore would otherwise find taken)
$rr->query_safe(
	q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM q.u});
my $before = sql($counts);
my $old = sql(q{SELECT pg_relation_filenode('q.t')});
sql("SELECT dolt_reset('--hard', '$a')");
cmp_ok(sql($counts), '>', $before,
	'with an older snapshot held, the head\'s pages are given back');
is(sql('SELECT sum(v) FROM q.t'), '0', 'the reset reached the commit');
is($rr->query_safe('SELECT sum(v) FROM q.t'),
	'5000', 'the older snapshot still reads what it saw');
is($rr->query_safe(q{SET enable_seqscan = off; SELECT v FROM q.t WHERE id = 7}),
	'5', 'through the table\'s index too');
is(sql(qq{SELECT test_bmap_exists($old)}), 't', 'the storage replaced is kept');
# (a keep while the older snapshot is held lets go of nothing it reads)
sql(q{UPDATE q.t SET v = 1 WHERE id = 2; SELECT dolt_commit('-am', 'e')});
is(sql(qq{SELECT test_bmap_exists($old)}), 't',
	'and still after a keep made while the older snapshot is held');
is($rr->query_safe('SELECT sum(v) FROM q.t'), '5000', 'which reads it still');
$rr->query(q{UPDATE q.t SET v = 6 WHERE id = 1});
like($rr->{stderr},
	qr/could not serialize access due to a concurrent move of relation "t"/,
	'the older snapshot can\'t change it');
$rr->{stderr} = '';
$rr->query_safe('ROLLBACK');
is($rr->query_safe('SELECT sum(v) FROM q.t'), '1',
	'a later transaction reads the commit\'s rows (and e\'s)');
$rr->quit;
# (the storage replaced is let go of by the next keep, no snapshot older
# than the reset left: the branch's entry for it keeps it until then)
is(sql(qq{SELECT test_bmap_exists($old)}), 't',
	'the storage replaced is kept by the branch');
sql(q{UPDATE q.t SET v = 1 WHERE id = 1; SELECT dolt_commit('-am', 'd')});
is(sql(qq{SELECT test_bmap_exists($old)}), 'f',
	'and let go of by the next keep, no snapshot older than the reset left');
sql("SELECT dolt_reset('--hard', '$a')");

# With no older snapshot, the head's pages are given back.
cycle();
sql(q{UPDATE q.t SET v = 5});
$before = sql($counts);
sql("SELECT dolt_reset('--hard', '$a')");
cmp_ok(sql($counts), '>', $before,
	'with no older snapshot, the head\'s pages are given back');
is(sql('SELECT sum(v) FROM q.t'), '0', 'and the reset reached the commit');

# On a branch other than main, only a snapshot of a session on the branch
# holds the head path back: a session on another branch reads other
# relations.  (Nobody holds main's lock, so on main every snapshot counts.)
# (main back at c, so that no state but side's keeps a's pages: a reset
# could otherwise be given them by a share, which isn't the head path)
sql("SELECT dolt_reset('--hard', '$c')");
sql(q{SELECT dolt_branch('side', dolt_hashof('HEAD'))});
sub on_side { return sql("SELECT dolt_checkout('side'); $_[0]"); }
# (back and forth on side, and a reset of a dirty working set to the head,
# whose restore gives the table new storage: a's kept pages are gone)
on_side("SELECT dolt_reset('--hard', '$c'); SELECT dolt_reset('--hard', '$a');
	SELECT dolt_reset('--hard', '$c'); UPDATE q.t SET v = 7;
	SELECT dolt_reset('--hard', '$c'); UPDATE q.t SET v = 5");
my $main = $node->background_psql('postgres');
$main->query_safe(
	q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM q.u});
$before = sql($counts);
on_side("SELECT dolt_reset('--hard', '$a')");
cmp_ok(sql($counts), '>', $before,
	'a snapshot on another branch doesn\'t hold the head path back');
$main->query_safe('COMMIT');
$main->quit;

on_side("SELECT dolt_reset('--hard', '$c'); SELECT dolt_reset('--hard', '$a');
	SELECT dolt_reset('--hard', '$c'); UPDATE q.t SET v = 7;
	SELECT dolt_reset('--hard', '$c'); UPDATE q.t SET v = 5");
my $side = $node->background_psql('postgres');
$side->query_safe(q{SELECT dolt_checkout('side')});
$side->query_safe(
	q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM q.u});
$before = sql($counts);
on_side("SELECT dolt_reset('--hard', '$a')");
cmp_ok(sql($counts), '>', $before,
	'nor does a snapshot on the branch');
is($side->query_safe('SELECT sum(v) FROM q.t'),
	'5000', 'which still reads what it saw');
$side->query_safe('COMMIT');
$side->quit;

# A snapshot taken off the branch, its transaction coming to the branch
# after the move: refused, as REPEATABLE READ refuses a conflict, rather
# than reading the head's rows.
on_side("SELECT dolt_reset('--hard', '$c'); SELECT dolt_reset('--hard', '$a');
	SELECT dolt_reset('--hard', '$c'); UPDATE q.t SET v = 7;
	SELECT dolt_reset('--hard', '$c'); UPDATE q.t SET v = 5");
my $late = $node->background_psql('postgres', on_error_stop => 0);
$late->query_safe(
	q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM q.u});
$before = sql($counts);
on_side("SELECT dolt_reset('--hard', '$a')");
cmp_ok(sql($counts), '>', $before, 'the head\'s pages are given back on side');
$late->query(q{SELECT dolt_checkout('side')});
like($late->{stderr},
	qr/could not serialize access: branch "side" was given its head's kept pages/,
	'a transaction whose snapshot is older can\'t come to the branch');
$late->{stderr} = '';
$late->query_safe('ROLLBACK');
# (a new transaction comes to it, and reads the commit's rows)
$late->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ});
$late->query_safe(q{SELECT dolt_checkout('side')});
is($late->query_safe('SELECT sum(v) FROM q.t'), '0',
	'a later one comes to it, and reads the commit\'s rows');
$late->query_safe('COMMIT');
$late->quit;

# main dirty again, its head's pages to be given back by a reset to a
sub dirty_main
{
	cycle();
	sql(q{UPDATE q.t SET v = 5});
}

# A snapshot imported into a transaction whose backend built the
# relation's entry after the reset (its storage the new one): it reads
# what it saw all the same, as does the exporting transaction, and a
# parallel scan of it.
dirty_main();
my $exporter = $node->background_psql('postgres');
$exporter->query_safe(
	q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM q.u});
my $exported = $exporter->query_safe(q{SELECT pg_export_snapshot()});
sql("SELECT dolt_reset('--hard', '$a')");
is($exporter->query_safe('SELECT sum(v) FROM q.t'), '5000',
	'the exporting snapshot reads what it saw');
my $importer = $node->background_psql('postgres');
is($importer->query_safe('SELECT sum(v) FROM q.t'), '0',
	'a backend reads the reset\'s rows, its entry built after it');
$importer->query_safe(qq{BEGIN ISOLATION LEVEL REPEATABLE READ;
	SET TRANSACTION SNAPSHOT '$exported'});
is($importer->query_safe('SELECT sum(v) FROM q.t'), '5000',
	'the snapshot imported there reads what it saw');
$importer->query_safe(q{SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;
	SET min_parallel_table_scan_size = 0; SET debug_parallel_query = on});
is($importer->query_safe('SELECT sum(v) FROM q.t'), '5000',
	'and so does a parallel scan of it');
$importer->query_safe('COMMIT');
$importer->quit;
$exporter->query_safe('COMMIT');
$exporter->quit;

# A crash with the storage replaced still kept: the first keep after the
# restart drops it (no snapshot outlives one).
dirty_main();
$rr = $node->background_psql('postgres');
$rr->query_safe(
	q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM q.u});
$old = sql(q{SELECT pg_relation_filenode('q.t')});
sql("SELECT dolt_reset('--hard', '$a')");
$node->stop('immediate');
eval { $rr->quit; };
$node->start;
is(sql(qq{SELECT test_bmap_exists($old)}), 't',
	'the storage replaced survives a crash');
sql(q{UPDATE q.t SET v = 1 WHERE id = 3; SELECT dolt_commit('-am', 'f')});
is(sql(qq{SELECT test_bmap_exists($old)}), 'f',
	'and the first keep after it drops it');

# A branch deleted with the storage a reset replaced still kept: dropped
# with the branch.
sql(q{SELECT dolt_branch('gone', dolt_hashof('HEAD'))});
sub on_gone { return sql("SELECT dolt_checkout('gone'); $_[0]"); }
on_gone("SELECT dolt_reset('--hard', '$c'); SELECT dolt_reset('--hard', '$a');
	SELECT dolt_reset('--hard', '$c'); UPDATE q.t SET v = 5");
my $on = $node->background_psql('postgres');
$on->query_safe(q{SELECT dolt_checkout('gone')});
$on->query_safe(
	q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM q.u});
my @out = split /\n/, on_gone(q{SELECT pg_relation_filenode('q.t')});
$old = $out[-1];
on_gone("SELECT dolt_reset('--hard', '$a')");
is(sql(qq{SELECT test_bmap_exists($old)}), 't',
	'the storage a reset of a branch replaced is kept');
$on->query_safe(q{COMMIT; SELECT dolt_checkout('main')});
$on->quit;
sql(q{SELECT dolt_branch('-D', 'gone')});
is(sql(qq{SELECT test_bmap_exists($old)}), 'f',
	'and dropped as the branch is deleted');

# With no entry free to name the storage replaced (as if every one were
# still needed), the reset doesn't restore, writing the rows instead, so
# the older snapshot still reads what it saw.
SKIP:
{
	skip 'injection points not supported by this build', 3
	  unless $ENV{enable_injection_points} eq 'yes';
	sql(q{CREATE EXTENSION injection_points});
	# (side, which keeps a's pages, deleted: the reset could otherwise be
	# given them by a share, which isn't the head path)
	sql(q{SELECT dolt_branch('-D', 'side')});
	dirty_main();
	$rr = $node->background_psql('postgres');
	$rr->query_safe(
		q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM q.u});
	sql(q{SELECT injection_points_attach('prolly-replaced-storage-full', 'notice')});
	$before = sql($counts);
	sql("SELECT dolt_reset('--hard', '$a')");
	sql(q{SELECT injection_points_detach('prolly-replaced-storage-full')});
	is(sql($counts), $before, 'with no entry free, nothing is restored');
	is(sql('SELECT sum(v) FROM q.t'), '0', 'the reset reached the commit');
	is($rr->query_safe('SELECT sum(v) FROM q.t'), '5000',
		'and the older snapshot reads what it saw');
	$rr->query_safe('COMMIT');
	$rr->quit;
}

# Two hard resets of a dirty working set, one after the other, no
# snapshot older than either: each one's replaced storage is let go of by
# the keeps after, not one of them left behind (an entry of replaced
# storage is told from another by its file number, all having the same
# id).
sql(q{SELECT dolt_reset('--hard')});
my $first = sql(q{SELECT pg_relation_filenode('q.t')});
sql(q{UPDATE q.t SET v = 7; SELECT dolt_reset('--hard')});
my $second = sql(q{SELECT pg_relation_filenode('q.t')});
sql(q{UPDATE q.t SET v = 8; SELECT dolt_reset('--hard')});
sql(q{UPDATE q.t SET v = 1 WHERE id = 4; SELECT dolt_commit('-am', 'g')});
sql(q{UPDATE q.t SET v = 1 WHERE id = 5; SELECT dolt_commit('-am', 'h')});
is(sql(qq{SELECT test_bmap_exists($first)}), 'f',
	'the first reset\'s replaced storage is dropped');
is(sql(qq{SELECT test_bmap_exists($second)}), 'f',
	'and the second\'s');

$node->stop;
done_testing();
