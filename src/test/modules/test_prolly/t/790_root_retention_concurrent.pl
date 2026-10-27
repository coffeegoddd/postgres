# Copyright (c) 2026, PostgreSQL Global Development Group

# Two branches' retention passes at once.  Both keep a root of a commit
# that only a third branch's window holds: each may let go of its own on
# the strength of the other's, but not both.  A pass that does holds the
# other branch until it commits, so the other's pass waits, then finds the
# first's root let go of, and keeps its own.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('root_retention_concurrent');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	ALTER DATABASE postgres SET prolly.restore_min_changes = 0});
sql(q{CREATE SCHEMA rc VERSIONED;
	CREATE TABLE rc.t (id int PRIMARY KEY, v int NOT NULL);
	INSERT INTO rc.t SELECT g, 0 FROM generate_series(1, 4000) g});
for my $i (1 .. 3)
{
	sql(qq{UPDATE rc.t SET v = $i WHERE id % 10 = $i;
		SELECT dolt_commit('-Am', 'c0$i')});
}
my $c03 = sql(q{SELECT dolt_hashof('HEAD')});

# bb made at c03 and used (keeping c03), then moved on; cc made at c03,
# never used; main moved on
sql(q{SELECT dolt_branch('bb'); SELECT dolt_branch('cc')});
sql(q{SELECT dolt_checkout('bb');
	UPDATE rc.t SET v = 11 WHERE id % 10 = 1; SELECT dolt_commit('-am', 'b1');
	UPDATE rc.t SET v = 12 WHERE id % 10 = 2; SELECT dolt_commit('-am', 'b2');
	UPDATE rc.t SET v = 13 WHERE id % 10 = 3; SELECT dolt_commit('-am', 'b3')});
for my $i (1 .. 3)
{
	sql(qq{UPDATE rc.t SET v = 20 + $i WHERE id % 10 = $i;
		SELECT dolt_commit('-am', 'm$i')});
}
# (no window till now, so every root kept: then a window of two commits)
sql(q{ALTER DATABASE postgres SET prolly.history_min_commits = 2});
my $keepers = qq{SELECT
	(SELECT count(*) FROM test_kept_commits('main') WHERE commit_id = '$c03') +
	(SELECT count(*) FROM test_kept_commits('bb') WHERE commit_id = '$c03')};
is(sql($keepers), '2', 'main and bb both keep c03, only cc\'s window holds it');

# main's pass held where it found bb's root, bb's pass meanwhile
my $a = $node->background_psql('postgres');
$a->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-root-kept-elsewhere', 'wait')});
$a->query_until(qr/started/, q{
	\echo started
	SELECT test_retain_roots('main');
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-root-kept-elsewhere'})
  or die 'main\'s pass did not wait';
my $b = $node->background_psql('postgres');
$b->query_until(qr/started/, q{
	\echo started
	SELECT test_retain_roots('bb');
	\echo done
});
# (bb's pass, given a moment: it waits for main's, which holds bb)
usleep(2_000_000);
sql(q{SELECT injection_points_wakeup('prolly-root-kept-elsewhere')});
$a->query_until(qr/done/, '');
$b->query_until(qr/done/, '');
$a->quit;
$b->quit;
is(sql($keepers), '1', 'one of them still keeps c03');

# so cc's first use, and its move to c03, write no rows
my $applied = q{SELECT pg_stat_force_next_flush();
	SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = 'apply_rows'), 0)};
my $before = sql($applied);
sql(q{SELECT dolt_checkout('cc')});
is(sql($applied), $before, 'cc is made of c03\'s root, no row written');

$node->stop;
done_testing();
