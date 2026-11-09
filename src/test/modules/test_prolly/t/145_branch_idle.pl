# Copyright (c) 2026, PostgreSQL Global Development Group

# With branch_idle_timeout set, a branch's working set is given back once it has gone unused that
# long, counted from when the last session left it, by a checkout or a savepoint rolled back, its
# head kept, and it's given one again row for row.  One a session is on, one with changes not
# committed, a database's or role's default_branch, and "main" keep theirs; "main" isn't given back
# on request either, so that there's always one to make another from.  One whose writers can't be
# held off, as a prepared transaction's, keeps its working set without keeping the others from
# being given back.  Set back to 0, nothing is given back.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time usleep);

# (no session of the test's checks out the database's default_branch)
$ENV{PGOPTIONS} = '-c default_branch=';

my $node = PostgreSQL::Test::Cluster->new('branch_idle');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\nmax_prepared_transactions = 2\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on { return "SET catalog_branch = $_[0]; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n"; }
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', on($branch) . $sql);
}
sub rows { return sql($_[0], q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t}); }
sub given_back { return $node->safe_psql('postgres', qq{SELECT test_branch_of('$_[0]') IS NULL}) eq 't'; }

# (until a worker giving back working sets has been through every branch)
sub pass_done
{
	return $node->poll_query_until('postgres',
		q{SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'branch idle worker'});
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; GRANT USAGE ON SCHEMA app TO PUBLIC;
	SET ROLE app_owner; CREATE TABLE app.t (id integer PRIMARY KEY, v text); GRANT SELECT ON app.t TO PUBLIC;
	INSERT INTO app.t SELECT g, 'c1' FROM generate_series(1, 5) g});
sql($main, "SELECT test_branch_link('main', $main)");
sql($main, q{SELECT test_branch_commit('main', 'c1')});
my $c1_rows = rows($main);

# "main" keeps its working set, on request too.
my ($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_dematerialize('main')});
like($err, qr/branch "main" can't be dematerialized/, '"main" isn\'t given back on request');

# (in this order, so the one that can't be given back is looked at before those that can)
my @names = qw(idle prep late on dirty def ddef left sp);
$node->safe_psql('postgres', q{SELECT test_branch_make(b, 'main') FROM unnest(ARRAY['}
	. join("', '", @names) . q{']) b});
my %id = map { $_ => $node->safe_psql('postgres', qq{SELECT test_branch_materialize('$_')}) } @names;

# One a session is on, one with changes not committed, a role's and a database's default_branch, and
# one with a prepared transaction's writes.
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT test_branch_checkout('on')});
sql($id{dirty}, q{SET ROLE app_owner; INSERT INTO app.t VALUES (6, 'uncommitted')});
$node->safe_psql('postgres', q{CREATE ROLE away; ALTER ROLE away SET default_branch = 'def'});
$node->safe_psql('postgres', q{ALTER DATABASE postgres SET default_branch = 'ddef'});
sql($id{prep}, q{BEGIN; SET LOCAL ROLE app_owner; INSERT INTO app.t VALUES (7, 'prepared'); PREPARE TRANSACTION 'p'});

# Ones a session stays on past the time, then leaves: by a checkout, and by a savepoint rolled back.
my $l = $node->background_psql('postgres');
$l->query_safe(q{SELECT test_branch_checkout('left')});
my $p = $node->background_psql('postgres');
$p->query_safe(q{BEGIN; SAVEPOINT a; SELECT test_branch_checkout('sp')});

my $timeout_s = 8;
$node->append_conf('postgresql.conf', "branch_idle_timeout = ${timeout_s}s\n");
$node->reload;

my $deadline = time + $PostgreSQL::Test::Utils::timeout_default;
usleep(200_000) until given_back('idle') || time > $deadline;
ok(given_back('idle'), 'a branch gone unused has its working set given back');
is($node->safe_psql('postgres', q{SELECT test_branch_head('idle') IS NOT NULL}), 't', 'its head kept');

# (the worker now holding off the writers of the one with a prepared transaction's, until it gives up)
is($node->safe_psql('postgres', q{SET lock_timeout = '2s'; ALTER ROLE away SET work_mem = '1MB'; SELECT 'set'}), 'set',
	'a role\'s settings change while the worker waits on a branch\'s writers');
ok(pass_done(), 'the worker goes through the rest');
ok(given_back('late'), 'and gives back one after the one it couldn\'t, in the same pass');
ok(!given_back('on'), 'one a session is on keeps its working set');
ok(!given_back('left') && !given_back('sp'), 'and ones sessions are still on, past the time');
ok(!given_back('main'), 'and "main"');
ok(!given_back('prep'), 'and one whose writers can\'t be held off');
like(slurp_file($node->logfile), qr/could not hold off the writers of catalog branch $id{prep} in time/,
	'which is reported');

# Its time counts from when it was left, not from when it was checked out.
$l->query_safe(q{SELECT test_branch_checkout('main')});
$p->query_safe(q{ROLLBACK TO a; COMMIT});
my $left_at = time;
usleep(200_000) until (given_back('left') && given_back('sp')) || time > $deadline;
my $after = time - $left_at;
ok(given_back('left'), 'one left is given back once it has gone unused, after one that can\'t be');
ok(given_back('sp'), 'and one left by a savepoint rolled back');
cmp_ok($after, '>=', $timeout_s - 1, 'no sooner than the time after they were left');
ok(!given_back('dirty'), 'one with changes not committed keeps its working set');
ok(!given_back('def') && !given_back('ddef'), 'and a role\'s and a database\'s default_branch');
ok(!given_back('prep'), 'and one whose writers can\'t be held off still');
unlike(slurp_file($node->logfile), qr/branch idle worker.*ERROR:  branch "d?def" is a default_branch/,
	'a default_branch kept without an error');

# Given one again, row for row.
is(rows($node->safe_psql('postgres', q{SELECT test_branch_materialize('idle')})), $c1_rows,
	'given one again, a branch has its head\'s rows');

# Its changes committed, and no longer a default_branch, each is given back too.
sql($id{dirty}, q{SELECT test_branch_commit('dirty', 'c2')});
$node->safe_psql('postgres', q{ALTER ROLE away RESET default_branch; ALTER DATABASE postgres RESET default_branch});
$deadline = time + $PostgreSQL::Test::Utils::timeout_default;
usleep(200_000) until (given_back('dirty') && given_back('def') && given_back('ddef')) || time > $deadline;
ok(given_back('dirty'), 'one whose changes are committed is given back');
ok(given_back('def') && given_back('ddef'), 'and ones no longer a default_branch');
ok(!given_back('on'), 'one a session is on still keeps its working set');
$node->safe_psql('postgres', q{COMMIT PREPARED 'p'});

# Set back to 0, nothing is given back.
$node->append_conf('postgresql.conf', "branch_idle_timeout = 0\n");
$node->reload;
ok(pass_done(), 'the last worker done');
$node->safe_psql('postgres', q{SELECT test_branch_materialize('idle')});
usleep(($timeout_s + 8) * 1_000_000);
ok(!given_back('idle'), 'turned off, a branch gone unused keeps its working set');

$_->quit for ($s, $l, $p);
done_testing();
