# Copyright (c) 2026, PostgreSQL Global Development Group

# With branch_idle_timeout set, a branch's working set is given back once it has gone unused that
# long, counted from when the last session left it, its head kept, and it's given one again row for
# row.  One a session is on, one with changes not committed, a default_branch, and "main" keep theirs;
# "main" isn't given back on request either, so that there's always one to make another from.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time usleep);

my $node = PostgreSQL::Test::Cluster->new('branch_idle');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\n");
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

$node->safe_psql('postgres', q{SELECT test_branch_make(b, 'main') FROM unnest(ARRAY['idle', 'on', 'dirty', 'def', 'left']) b});
my %id = map { $_ => $node->safe_psql('postgres', qq{SELECT test_branch_materialize('$_')}) } qw(idle on dirty def left);

# One a session is on, one with changes not committed, and a role's default_branch.
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT test_branch_checkout('on')});
sql($id{dirty}, q{SET ROLE app_owner; INSERT INTO app.t VALUES (6, 'uncommitted')});
$node->safe_psql('postgres', q{CREATE ROLE away; ALTER ROLE away SET default_branch = 'def'});

# One a session stays on past the time, then leaves.
my $l = $node->background_psql('postgres');
$l->query_safe(q{SELECT test_branch_checkout('left')});

my $timeout_s = 8;
$node->append_conf('postgresql.conf', "branch_idle_timeout = ${timeout_s}s\n");
$node->reload;

my $deadline = time + $PostgreSQL::Test::Utils::timeout_default;
usleep(200_000) until given_back('idle') || time > $deadline;
ok(given_back('idle'), 'a branch gone unused has its working set given back');
is($node->safe_psql('postgres', q{SELECT test_branch_head('idle') IS NOT NULL}), 't', 'its head kept');
ok(!given_back('on'), 'one a session is on keeps its working set');
ok(!given_back('left'), 'and one a session is still on, past the time');
ok(!given_back('main'), 'and "main"');

# Its time counts from when it was left, not from when it was checked out.
$l->query_safe(q{SELECT test_branch_checkout('main')});
my $left_at = time;
usleep(200_000) until given_back('left') || time > $deadline;
my $after = time - $left_at;
ok(given_back('left'), 'one left is given back once it has gone unused');
cmp_ok($after, '>=', $timeout_s - 1, 'no sooner than the time after it was left');
ok(!given_back('dirty'), 'one with changes not committed keeps its working set');
ok(!given_back('def'), 'and a default_branch');

# Given one again, row for row.
is(rows($node->safe_psql('postgres', q{SELECT test_branch_materialize('idle')})), $c1_rows,
	'given one again, a branch has its head\'s rows');

# Its changes committed, and no longer a default_branch, each is given back too.
sql($id{dirty}, q{SELECT test_branch_commit('dirty', 'c2')});
$node->safe_psql('postgres', q{ALTER ROLE away RESET default_branch});
$deadline = time + $PostgreSQL::Test::Utils::timeout_default;
usleep(200_000) until (given_back('dirty') && given_back('def')) || time > $deadline;
ok(given_back('dirty'), 'one whose changes are committed is given back');
ok(given_back('def'), 'and one no longer a default_branch');
ok(!given_back('on'), 'one a session is on still keeps its working set');

$_->quit for ($s, $l);
done_testing();
