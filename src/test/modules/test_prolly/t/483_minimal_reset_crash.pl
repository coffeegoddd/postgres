# Copyright (c) 2026, PostgreSQL Global Development Group

# At wal_level minimal, a share or a snapshot of block-mapped storage (a
# branch's copy, a kept commit) logs its own blocks as images, as at the
# levels for backups; a sync of the page store would not survive the
# replay.  Redo of the records that gave those blocks to the relation
# zeroes them again, and the records that made the pages are skipped once
# the relation was dropped or gave the block up: only the images, replayed
# after them, give the copies their pages after a crash.  Each case writes
# after a checkpoint, so that recovery replays the blocks' allocations,
# then crashes and reads every page of a table, its index, its change log
# and the log's index, as of the head and of an earlier commit.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('minimal_reset_crash');
$node->init;
$node->append_conf('postgresql.conf', qq{
wal_level = minimal
max_wal_senders = 0
autovacuum = off
checkpoint_timeout = '1h'
prolly.restore_min_changes = 100000000
});
$node->start;

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }

sub crash { $node->stop('immediate'); $node->start; }

sql(q{CREATE EXTENSION amcheck; CREATE SCHEMA app VERSIONED});

# Every page of table $t, its index, its change log and the log's index,
# read: the rows counted by a scan of each, each btree checked.
sub check
{
	my ($t, $what, $db) = @_;
	my ($ret, $out, $err) = $node->psql($db // 'postgres', qq{
		SELECT count(*), sum(v) FROM app.$t;
		SET enable_seqscan = off;
		SELECT count(*) FROM app.$t WHERE id > 0;
		RESET enable_seqscan;
		SELECT bool_and(bt_index_check(i.indexrelid, true) IS NOT NULL)
			FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
			WHERE i.indrelid = 'app.$t'::regclass
			   OR c.relname = 'pg_prolly_log_' || 'app.$t'::regclass::oid || '_index';
	});
	is($ret, 0, "$what: every page reads") or diag($err);
	return $out;
}

# A hard reset, writes, a crash, then a hard reset again.
sql(q{CREATE TABLE app.r (id int PRIMARY KEY, v int NOT NULL); CHECKPOINT;
	INSERT INTO app.r SELECT g, 0 FROM generate_series(1, 3000) g});
my $a = sql(q{SELECT (dolt_commit('-Am', 'a'))[1]});
sql(q{UPDATE app.r SET v = 1 WHERE id <= 300; SELECT dolt_commit('-am', 'b')});
sql(qq{UPDATE app.r SET v = 2 WHERE id % 5 = 0; SELECT dolt_reset('--hard', '$a')});
sql(q{UPDATE app.r SET v = 9 WHERE id % 3 = 0});
crash();
is(check('r', 'after a reset and a crash'), "3000|9000\n3000\nt",
	'the writes after the reset');
my ($ret, $out, $err) =
  $node->psql('postgres', qq{SELECT dolt_reset('--hard', '$a')});
is($ret, 0, 'a hard reset after the crash succeeds') or diag($err);
is(check('r', 'reset after the crash'), "3000|0\n3000\nt",
	'the table reads back as the commit');

# A commit that folds the change log, then a hard reset of later writes,
# writes, a crash, then a reset to the fold.
sql(q{CREATE TABLE app.f (id int PRIMARY KEY, v int NOT NULL); CHECKPOINT;
	INSERT INTO app.f SELECT g, 0 FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'f1');
	UPDATE app.f SET v = 1 WHERE id <= 300});
my $f2 = sql(q{SELECT (dolt_commit('-am', 'f2'))[1]});
sql(q{UPDATE app.f SET v = 2 WHERE id % 7 = 0; SELECT dolt_reset('--hard');
	UPDATE app.f SET v = 3 WHERE id % 13 = 0});
crash();
sql(qq{SELECT dolt_reset('--hard', '$f2')});
is(check('f', 'reset to the fold'), "3000|300\n3000\nt", 'the fold\'s rows');

# A branch's first checkout, its copies made of main's pages; then main's
# storage replaced by a hard reset, a crash, and the branch read.
sql(q{CREATE TABLE app.c (id int PRIMARY KEY, v int NOT NULL); CHECKPOINT;
	INSERT INTO app.c SELECT g, 0 FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'c1'); SELECT dolt_branch('feature')});
sql(q{SELECT dolt_checkout('feature'); UPDATE app.c SET v = 3 WHERE id <= 100});
sql(q{UPDATE app.c SET v = 4 WHERE id % 9 = 0; SELECT dolt_reset('--hard');
	UPDATE app.c SET v = 5 WHERE id % 17 = 0});
crash();
is(check('c', 'the branch after a crash', 'postgres/feature'),
	"3000|300\n3000\nt", 'the branch\'s rows');
sql(q{SELECT dolt_reset('--hard')}, 'postgres/feature');
is(check('c', 'the branch reset', 'postgres/feature'), "3000|0\n3000\nt",
	'the branch reset to its commit');
sql(q{SELECT dolt_reset('--hard')});
is(check('c', 'main reset'), "3000|0\n3000\nt", 'main reset to its commit');

# A three-way merge, then a hard reset of later writes, writes, a crash,
# then main reset to the merge and to before it, and the other branch read.
sql(q{CREATE TABLE app.m (id int PRIMARY KEY, v int NOT NULL); CHECKPOINT;
	INSERT INTO app.m SELECT g, 0 FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'm1'); SELECT dolt_branch('side')});
sql(q{UPDATE app.m SET v = 5 WHERE id <= 100; SELECT dolt_commit('-am', 'side')},
	'postgres/side');
sql(q{UPDATE app.m SET v = 6 WHERE id > 2900; SELECT dolt_commit('-am', 'm2');
	SELECT dolt_merge('side', '-m', 'merged');
	UPDATE app.m SET v = 7 WHERE id % 11 = 0; SELECT dolt_reset('--hard');
	UPDATE app.m SET v = 8 WHERE id % 19 = 0});
crash();
sql(q{SELECT dolt_reset('--hard')});
is(check('m', 'main reset to the merge'), "3000|1100\n3000\nt",
	'the merge\'s rows');
sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(check('m', 'main reset to before the merge'), "3000|600\n3000\nt",
	'the rows from before the merge');
is(check('m', 'the merged branch', 'postgres/side'), "3000|500\n3000\nt",
	'the merged branch\'s rows');

$node->stop;
done_testing();
