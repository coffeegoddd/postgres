# Copyright (c) 2026, PostgreSQL Global Development Group

# Two sessions on two branches of one schema write the same table at once, its rows by the same keys
# and its columns, neither waiting for the other: each sees only its own, every index and heap stays
# right on both, and capture has each.  A branch's changes not committed are its working set's, not a
# session's: the next session on it, and the one after a restart, sees them.  A hot standby follows
# each branch made, written, given a working set and given it back, renamed and deleted, reading each
# as its primary does.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_at_once');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION amcheck');
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('branch_at_once_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on { return "SET catalog_branch = $_[0]; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n"; }
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', on($branch) . $sql);
}
my $rows = q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t};
sub rows { return sql($_[0], $rows); }

# How many B-tree indexes are checked (amcheck raises for one that isn't right), and what's wrong in
# each heap and TOAST table.
sub checks
{
	my ($branch) = @_;
	return sql($branch, q{SELECT count(*) FROM (SELECT c.oid FROM pg_class c JOIN pg_am a ON a.oid = c.relam
			WHERE c.relnamespace = 'app'::regnamespace AND c.relkind = 'i' AND a.amname = 'btree' OFFSET 0) x
			WHERE bt_index_check(x.oid, true, true) IS NOT NULL})
	  . '/' . sql($branch, q{SELECT count(*) FROM (SELECT r.oid FROM pg_class c,
			LATERAL (VALUES (c.oid), (nullif(c.reltoastrelid, 0))) r(oid)
			WHERE c.relnamespace = 'app'::regnamespace AND c.relkind = 'r' AND r.oid IS NOT NULL OFFSET 0) x,
			LATERAL verify_heapam(x.oid)});
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; GRANT USAGE ON SCHEMA app TO PUBLIC;
	SET ROLE app_owner; CREATE TABLE app.t (id integer PRIMARY KEY, v text); GRANT SELECT ON app.t TO PUBLIC;
	CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT g, 'c1' FROM generate_series(1, 1000) g});
sql($main, "SELECT test_branch_link('main', $main)");
sql($main, q{SELECT test_branch_commit('main', 'c1')});
my $c1_rows = rows($main);

# Two sessions, each on a branch of its own, in transactions open at once: the same rows updated
# and deleted, the same key inserted, a column added on one as the other writes, an index made on
# the other as the first does.
$node->safe_psql('postgres', qq{SELECT test_branch_create('$_', 'main')}) for qw(a b);
my %id = map { $_ => $node->safe_psql('postgres', qq{SELECT test_branch_of('$_')}) } qw(a b);
my %s = map { $_ => $node->background_psql('postgres') } qw(a b);
$s{$_}->query_safe(qq{SELECT test_branch_checkout('$_'); SET ROLE app_owner; BEGIN}) for qw(a b);
$s{a}->query_safe(q{UPDATE app.t SET v = 'a' WHERE id <= 100});
$s{b}->query_safe(q{UPDATE app.t SET v = 'b' WHERE id <= 100});
$s{a}->query_safe(q{DELETE FROM app.t WHERE id BETWEEN 101 AND 150});
$s{b}->query_safe(q{DELETE FROM app.t WHERE id BETWEEN 141 AND 200});
$s{a}->query_safe(q{INSERT INTO app.t VALUES (5000, 'a')});
$s{b}->query_safe(q{INSERT INTO app.t VALUES (5000, 'b')});
$s{a}->query_safe(q{ALTER TABLE app.t ADD COLUMN extra integer DEFAULT 7});
$s{b}->query_safe(q{INSERT INTO app.t SELECT g, 'b' FROM generate_series(6000, 6999) g});
$s{b}->query_safe(q{CREATE INDEX t_v_id ON app.t (v, id)});
$s{a}->query_safe(q{UPDATE app.t SET extra = id WHERE id > 900});
pass('two sessions on two branches write the same table at once, neither waiting');
is($s{a}->query_safe(q{SELECT count(*) FILTER (WHERE v = 'a') || '/' || count(*) FROM app.t}), '101/951',
	'each sees its own uncommitted rows');
is($s{b}->query_safe(q{SELECT count(*) FILTER (WHERE v = 'b') || '/' || count(*) FROM app.t}), '1101/1941',
	'and the other its own');
$s{$_}->query_safe('COMMIT') for qw(a b);

my $a_rows = $s{a}->query_safe($rows);
my $b_rows = $s{b}->query_safe($rows);
is($s{a}->query_safe(q{SELECT count(*) FROM app.t WHERE v = 'b'}) . '/' . $s{b}->query_safe(q{SELECT count(*) FROM app.t WHERE v = 'a'}),
	'0/0', 'committed, neither sees the other\'s rows');
is(sql($id{a}, q{SELECT count(*) FROM pg_attribute WHERE attrelid = 'app.t'::regclass AND attname = 'extra'})
	. sql($id{b}, q{SELECT count(*) FROM pg_attribute WHERE attrelid = 'app.t'::regclass AND attname = 'extra'}),
	'10', 'nor the other\'s column');
is(sql($id{a}, q{SELECT to_regclass('app.t_v_id') IS NULL}) . sql($id{b}, q{SELECT to_regclass('app.t_v_id') IS NOT NULL}),
	'tt', 'nor the other\'s index');
is(sql($id{a}, q{SELECT v FROM app.t WHERE id = 5000}) . sql($id{b}, q{SELECT v FROM app.t WHERE id = 5000}), 'ab',
	'each has its own row under the key both inserted');
is(sql($id{b}, q{SET enable_seqscan = off; SELECT count(*) FROM app.t WHERE v = 'b'}), '1101',
	'found through each one\'s index');
is(checks($id{a}), '2/0', 'every index and heap of one is right');
is(checks($id{b}), '3/0', 'and of the other');
is(rows($main), $c1_rows, 'and the branch they came from as it was');
is(checks($main), '2/0', 'its indexes and heap right');
for my $br (qw(a b))
{
	is(sql($id{$br}, q{SELECT test_capture_barrier('app')}), sql($id{$br}, q{SELECT test_ws_root_capture('app')}),
		"capture has $br\'s writes");
}
$s{$_}->quit for qw(a b);

# Changes not committed outlast the session that made them, and a restart.
$node->safe_psql('postgres', q{SELECT test_branch_make('p', 'main')});
my $w = $node->background_psql('postgres');
$w->query_safe(q{SELECT test_branch_checkout('p'); SET ROLE app_owner; UPDATE app.t SET v = 'p' WHERE id = 1;
	INSERT INTO app.t VALUES (7000, 'p')});
$w->quit;
my $next = q{SELECT test_branch_checkout('p'); SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t WHERE id IN (1, 7000)};
is((split /\n/, $node->safe_psql('postgres', $next))[-1], '1:p,7000:p',
	'changes not committed are seen by the next session on the branch');
$node->restart;
is((split /\n/, $node->safe_psql('postgres', $next))[-1], '1:p,7000:p', 'and after a restart');
my $p_rows = rows($node->safe_psql('postgres', q{SELECT test_branch_of('p')}));

# A standby follows each.
sub standby_of { return $standby->safe_psql('postgres', qq{SELECT test_branch_of('$_[0]')}); }
sub standby_rows { return $standby->safe_psql('postgres', "SET catalog_branch = $_[0]; $rows"); }
$node->wait_for_replay_catchup($standby);
for my $br (qw(a b p))
{
	my $branch = $node->safe_psql('postgres', qq{SELECT test_branch_of('$br')});
	is(standby_of($br), $branch, "a standby has branch $br linked as its primary has");
	is(standby_rows($branch), $br eq 'a' ? $a_rows : $br eq 'b' ? $b_rows : $p_rows, "and reads its rows");
}
is($standby->safe_psql('postgres', "SET catalog_branch = $id{b}; SET enable_seqscan = off; SELECT count(*) FROM app.t WHERE v = 'b'"),
	'1101', 'through its indexes');
is($standby->safe_psql('postgres', "SET catalog_branch = $id{a}; SELECT sum(extra) FROM app.t"),
	$node->safe_psql('postgres', on($id{a}) . 'SELECT sum(extra) FROM app.t'), 'with its columns');
is($standby->safe_psql('postgres', q{SELECT test_branch_head('p')}), $node->safe_psql('postgres', q{SELECT test_branch_head('p')}),
	'and each head');

# Given back, given one again, renamed and deleted: the standby follows.
sql($id{a}, q{SELECT test_branch_commit('a', 'a1')});
$node->safe_psql('postgres', q{SELECT test_branch_dematerialize('a')});
$node->safe_psql('postgres', q{SELECT test_branch_rename('b', 'b2')});
$node->safe_psql('postgres', q{SELECT test_branch_delete('p', true)});
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', q{SELECT test_branch_of('a') IS NULL AND test_branch_head('a') IS NOT NULL}), 't',
	'a standby has a branch given back as a head');
is($standby->safe_psql('postgres', q{SELECT test_branch_of('b') IS NULL AND test_branch_of('b2') = } . $id{b}), 't',
	'one renamed by its new name');
is(standby_rows($id{b}), $b_rows, 'reading its rows');
is($standby->safe_psql('postgres', q{SELECT test_branch_of('p') IS NULL AND test_branch_head('p') IS NULL}), 't',
	'and one deleted, not at all');
my $a2 = $node->safe_psql('postgres', q{SELECT test_branch_materialize('a')});
$node->wait_for_replay_catchup($standby);
is(standby_of('a'), $a2, 'given one again, the standby has it');
is(standby_rows($a2), $a_rows, 'row for row');

$standby->stop;
done_testing();
