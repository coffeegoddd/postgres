# Copyright (c) 2026, PostgreSQL Global Development Group

# A refs get is answered from what an earlier get read under the same refs
# root, and a backend's own edits carry that to the root they make.  Each
# get still sees what it saw when every get descended the tree: the
# transaction's own writes (a branch moved, a setting changed, a merge's
# state), what an abort or a subtransaction's rollback undid, and what
# another session committed since, all read at once.  One session reads
# throughout, so what it read before is there to be answered from.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('refs_gets_fresh');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
fsync = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 10) g;
	SELECT dolt_commit('-Am', 'load') IS NOT NULL;
	SELECT dolt_branch('side') IS NOT NULL});

my $s = $node->background_psql('postgres', on_error_stop => 1);
sub rq { my $out = $s->query_safe($_[0]); chomp $out; return $out; }

my $head = sql(q{SELECT dolt_hashof('main')});
is(rq(q{SELECT dolt_hashof('main')}), $head, 'the head, read once');

# The session's own commit, in a statement of its own and in a block: the
# next get, there and in the next transaction, reads the new head.
my $c1 = rq(q{INSERT INTO app.t VALUES (11, 'a');
	SELECT (dolt_commit('-Am', 'c1'))[1]});
isnt($c1, $head, 'a commit moved main');
is(rq(q{SELECT dolt_hashof('main')}), $c1, 'its own commit is read next');
rq(q{BEGIN});
is(rq(q{SELECT dolt_hashof('main')}), $c1, 'in a block, the head read');
my $c2 = rq(q{INSERT INTO app.t VALUES (12, 'b');
	SELECT (dolt_commit('-Am', 'c2'))[1]});
is(rq(q{SELECT dolt_hashof('main')}), $c2, 'in the block, its commit read');
rq(q{COMMIT});
is(rq(q{SELECT dolt_hashof('main')}), $c2, 'the block committed, its head read');

# An abort, and a subtransaction rolled back, undo the commit: the head
# read after them is the one before.
rq(q{BEGIN});
my $c3 = rq(q{INSERT INTO app.t VALUES (13, 'c');
	SELECT (dolt_commit('-Am', 'c3'))[1]});
is(rq(q{SELECT dolt_hashof('main')}), $c3, 'the uncommitted commit read');
rq(q{ROLLBACK});
is(rq(q{SELECT dolt_hashof('main')}), $c2, 'after an abort, the head before');
rq(q{BEGIN; SAVEPOINT s});
my $c4 = rq(q{INSERT INTO app.t VALUES (14, 'd');
	SELECT (dolt_commit('-Am', 'c4'))[1]});
is(rq(q{SELECT dolt_hashof('main')}), $c4, 'the savepoint\'s commit read');
rq(q{ROLLBACK TO s});
is(rq(q{SELECT dolt_hashof('main')}), $c2,
	'after a rollback to the savepoint, the head before');
rq(q{COMMIT});
is(rq(q{SELECT dolt_hashof('main')}), $c2,
	'after the block commits, still the head before');

# Another session's commit is read at once, in a block too (the refs as
# they are now, not as the block began).
my $c5 = sql(q{INSERT INTO app.t VALUES (15, 'e');
	SELECT (dolt_commit('-Am', 'c5'))[1]});
is(rq(q{SELECT dolt_hashof('main')}), $c5, "another session's commit read");
rq(q{BEGIN});
is(rq(q{SELECT dolt_hashof('main')}), $c5, 'in a block, the head read');
my $c6 = sql(q{INSERT INTO app.t VALUES (16, 'f');
	SELECT (dolt_commit('-Am', 'c6'))[1]});
is(rq(q{SELECT dolt_hashof('main')}), $c6,
	"in the block, another session's later commit read");
rq(q{COMMIT});

# A setting (one kept in the refs, put outside any branch's entries): the
# session's own change, an aborted one, and another session's.
my $policy = q{SELECT time_window FROM prolly_history_policy('side')};
my $before = rq($policy);
rq(q{SELECT dolt_history_window('side', 'window', '10 days')});
is(rq($policy), '240:00:00', 'its own setting read');
rq(q{BEGIN});
rq(q{SELECT dolt_history_window('side', 'window', '30 days')});
is(rq($policy), '720:00:00', 'its setting in a block read');
rq(q{ROLLBACK});
is(rq($policy), '240:00:00', 'after an abort, the setting before');
sql(q{SELECT dolt_history_window('side', 'window', '20 days')});
is(rq($policy), '480:00:00', "another session's setting read");
isnt($before, '480:00:00', 'the setting changed from what it was');

# A merge's state, set and cleared by the session in a block, and undone
# by an abort.
sql(q{SELECT dolt_checkout('side');
	UPDATE app.t SET v = 'side' WHERE id = 1;
	SELECT dolt_commit('-Am', 'on side') IS NOT NULL;
	SELECT dolt_checkout('main');
	UPDATE app.t SET v = 'main' WHERE id = 1;
	SELECT dolt_commit('-Am', 'on main') IS NOT NULL});
my $conflicts = q{SELECT count(*) FROM dolt_conflicts()};
is(rq($conflicts), '0', 'no merge, no conflicts');
rq(q{BEGIN});
is(rq(q{SELECT (dolt_merge('side'))[4]}), 'conflicts found', 'the merge conflicts');
is(rq($conflicts), '1', "the merge's conflicts read");
is(rq(q{SELECT count(*) FROM dolt_status() WHERE table_name = 'app.t'}) > 0 ? 1 : 0,
	1, 'the merged table is in the status');
rq(q{SELECT dolt_merge('--abort')});
is(rq($conflicts), '0', 'the merge aborted, no conflicts read');
is(rq(q{SELECT (dolt_merge('side'))[4]}), 'conflicts found', 'merged again');
is(rq($conflicts), '1', "the second merge's conflicts read");
rq(q{ROLLBACK});
is(rq($conflicts), '0', 'after an abort, no merge read');
rq(q{BEGIN});
is(rq(q{SELECT (dolt_merge('side'))[4]}), 'conflicts found',
	'a merge after the abort');
is(rq($conflicts), '1', 'its conflicts read');
rq(q{ROLLBACK});

$s->quit;
$node->stop;
done_testing();
