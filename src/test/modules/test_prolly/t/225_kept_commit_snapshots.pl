# Copyright (c) 2026, PostgreSQL Global Development Group

# Another branch's kept commit gives a table its pages only where every
# snapshot sees them as the commit's rows.  The pages are that branch's,
# with versions it made and deleted before the commit: a snapshot taken
# before the commit was kept, on the branch being moved or any other, could
# see those, rows that were never this branch's nor in any commit.  So a
# move shares a kept commit's pages only once the transaction that kept it
# precedes every snapshot in the cluster, and writes the rows otherwise; a
# first use is made of kept pages only if its snapshot sees them kept.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_commit_snapshots');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub counter
{
	sql('SELECT pg_stat_force_next_flush()');
	return sql("SELECT value FROM pg_stat_prolly WHERE name = '$_[0]'");
}

# tables given another state's pages: shared, or restored from a kept
# commit's roots (whichever branch kept them)
sub given
{
	return counter('move_tables_shared') + counter('relations_restored');
}

my $rows = q{SELECT count(*) || ':' || sum(v) FROM ks.t};

# main and b at c0; each round, main commits (its pages kept), then moves
# on, and b, changed, is reset to that commit
sql(q{
	CREATE SCHEMA ks VERSIONED;
	CREATE TABLE ks.t (id bigint PRIMARY KEY, v int NOT NULL, w text NOT NULL);
	INSERT INTO ks.t SELECT g, g % 10, md5(g::text) FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'c0');
	SELECT dolt_branch('b');
});

my $mover = $node->background_psql('postgres');
my $reader = $node->background_psql('postgres');
my $main = $node->background_psql('postgres');
$mover->query_safe(q{SELECT dolt_checkout('b')});
$reader->query_safe(q{SELECT dolt_checkout('b')});

# main commits round's c, from main's working set as it is (one row more
# made and deleted on the way), and changes after it
sub main_commits
{
	my ($round) = @_;
	$main->query_safe(qq{
		UPDATE ks.t SET v = v + 1 WHERE id <= 5000;
		SELECT dolt_commit('-am', 'r$round');
		UPDATE ks.t SET v = v + 2 WHERE id <= 5000;
	});
	return $main->query_safe(
		qq{SELECT commit_hash FROM dolt_log WHERE message = 'r$round'});
}

# (b's rows at each round's commit: c0's, with ids <= 5000 one more each
# round, two more each round after it: 20000 rows, v summed)
sub at_round
{
	my ($round) = @_;
	my $sum = 90000 + 5000 * (3 * ($round - 1) + 1);
	return "20000:$sum";
}

# 1. a reader on b whose snapshot is older than the commit's keeping, but
# hasn't read the table: the reset writes the rows, and the reader never
# sees main's row 777777 (made and deleted on main before the commit)
$mover->query_safe(q{UPDATE ks.t SET v = 0 WHERE id <= 6000});
$main->query_safe(q{INSERT INTO ks.t VALUES (777777, 7, 'main only')});
$reader->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class WHERE false});
$main->query_safe(q{DELETE FROM ks.t WHERE id = 777777});
my $c1 = main_commits(1);
my $shared = given();
my $applied = counter('apply_rows');
$mover->query_safe(qq{SELECT dolt_reset('--hard', '$c1'); SELECT pg_stat_force_next_flush()});
is(given(), $shared, 'an older snapshot open: no kept pages shared');
cmp_ok(counter('apply_rows'), '>', $applied, 'the rows written');
is($mover->query_safe($rows), at_round(1), 'the reset branch has the commit\'s rows');
is($reader->query_safe(q{SELECT count(*) FROM ks.t WHERE id = 777777}), '0',
	'the older snapshot sees no row of main\'s that was in no commit');
$reader->query_safe('COMMIT');
is($reader->query_safe($rows), at_round(1), 'and after, the commit\'s rows');

# with no older snapshot, the same reset shares the kept pages
$mover->query_safe(q{UPDATE ks.t SET v = 0 WHERE id <= 6000});
my $c2 = main_commits(2);
$shared = given();
$mover->query_safe(qq{SELECT dolt_reset('--hard', '$c2'); SELECT pg_stat_force_next_flush()});
cmp_ok(given(), '>', $shared, 'no older snapshot: the kept pages shared');
is($mover->query_safe($rows), at_round(2), 'with the commit\'s rows');

# 2. the mover's own snapshot is older than the keeping (repeatable read,
# then serializable): the rows are written, and its reads are the commit's
foreach my $level ('REPEATABLE READ', 'SERIALIZABLE')
{
	my $round = $level eq 'SERIALIZABLE' ? 4 : 3;

	$mover->query_safe(q{UPDATE ks.t SET v = 0 WHERE id <= 6000});
	$mover->query_safe(qq{BEGIN ISOLATION LEVEL $level; SELECT count(*) FROM ks.t});
	my $c = main_commits($round);
	$shared = given();
	$mover->query_safe(qq{SELECT dolt_reset('--hard', '$c')});
	is($mover->query_safe($rows), at_round($round),
		lc($level) . ': the reset reads the commit\'s rows');
	$mover->query_safe('COMMIT');
	$mover->query_safe('SELECT pg_stat_force_next_flush()');
	is(given(), $shared, lc($level) . ': no kept pages shared');
	is($mover->query_safe($rows), at_round($round), lc($level) . ': and after');
}

# 3. a first use at a commit main kept anew after the user's snapshot (a
# reset of main back to it gives its pages back and keeps them again): not
# made of those pages
my $c5 = main_commits(5);
$main->query_safe(qq{SELECT dolt_branch('f', '$c5')});
my $user = $node->background_psql('postgres', on_error_stop => 0);
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$main->query_safe(q{SELECT dolt_reset('--hard')});
my $kept = counter('first_use_tables_kept');
$user->query(q{SELECT dolt_checkout('f')});
if ($user->{stderr} eq '')
{
	is($user->query_safe($rows), at_round(5), 'the first use has the commit\'s rows');
}
else
{
	like($user->{stderr}, qr/could not serialize access/, 'or the first use is refused');
	$user->{stderr} = '';
}
$user->query_safe('ROLLBACK');
$user->query_safe('SELECT pg_stat_force_next_flush()');
is(counter('first_use_tables_kept'), $kept,
	'pages kept after the snapshot: the first use isn\'t made of them');
$user->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$user->query_safe(q{SELECT dolt_checkout('f')});
is($user->query_safe($rows), at_round(5), 'retried, the commit\'s rows');
$user->query_safe('COMMIT');
$user->query_safe('SELECT pg_stat_force_next_flush()');
cmp_ok(counter('first_use_tables_kept'), '>', $kept, 'made of the pages kept');

$_->quit for ($mover, $reader, $main, $user);
$node->stop;
done_testing();
