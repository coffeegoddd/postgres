# Copyright (c) 2026, PostgreSQL Global Development Group

# Two sessions on two branches of one schema write the same table in transactions open at once, its
# rows by the same keys, its columns and its indexes, neither waiting for the other; and two write
# every page they share, values stored out of line too, in statements running at the same time.
# Each sees only its own, every index and heap stays right on each, TOAST tables and their indexes
# too, the branch they came from is as it was, and capture has each.  A branch's changes not
# committed are its working set's, not a session's: a session that leaves the branch and comes back,
# the next session on it, and one after a restart or a crash, sees them, and its head stays where it
# was.  A hot standby follows each branch made, written, committed, given back and given a working
# set again, renamed and deleted, reading each through its indexes as its primary does.

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
my $rows = q{SELECT string_agg(id || ':' || v || ':' || coalesce(md5(big), ''), ',' ORDER BY id) FROM app.t};
sub rows { return sql($_[0], $rows); }

# How many B-tree indexes are checked, TOAST tables' too (amcheck raises for one that isn't right),
# and what's wrong in each heap and TOAST table, the values stored out of line followed.
sub checks
{
	my ($branch) = @_;
	return sql($branch, q{SELECT count(*) FROM (SELECT c.oid FROM pg_class c JOIN pg_am a ON a.oid = c.relam
			WHERE c.relkind = 'i' AND a.amname = 'btree' AND (c.relnamespace = 'app'::regnamespace
				OR c.oid IN (SELECT i.indexrelid FROM pg_index i JOIN pg_class t ON t.reltoastrelid = i.indrelid
					WHERE t.relnamespace = 'app'::regnamespace)) OFFSET 0) x
			WHERE bt_index_check(x.oid, true, true) IS NOT NULL})
	  . '/' . sql($branch, q{SELECT count(*) FROM (SELECT r.oid FROM pg_class c,
			LATERAL (VALUES (c.oid), (nullif(c.reltoastrelid, 0))) r(oid)
			WHERE c.relnamespace = 'app'::regnamespace AND c.relkind = 'r' AND r.oid IS NOT NULL OFFSET 0) x,
			LATERAL verify_heapam(x.oid, check_toast => true)});
}

# Whether a query's plan reads through an index.
sub by_index
{
	my ($n, $branch, $query) = @_;
	return $n->safe_psql('postgres', "SET catalog_branch = $branch; SET enable_seqscan = off; EXPLAIN (COSTS OFF) $query")
	  =~ /Index|Bitmap/;
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; GRANT USAGE ON SCHEMA app TO PUBLIC;
	SET ROLE app_owner; CREATE TABLE app.t (id integer PRIMARY KEY, v text, big text); GRANT SELECT ON app.t TO PUBLIC;
	ALTER TABLE app.t ALTER COLUMN big SET STORAGE EXTERNAL;
	CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT g, 'c1', CASE WHEN g % 20 = 0 THEN repeat(md5(g::text), 300) END FROM generate_series(1, 1000) g});
sql($main, "SELECT test_branch_link('main', $main)");
sql($main, q{SELECT test_branch_commit('main', 'c1')});
my $c1 = $node->safe_psql('postgres', q{SELECT test_branch_head('main')});
my $c1_rows = rows($main);

# Two sessions, each on a branch of its own, in transactions open at once: the same rows updated
# and deleted, the same key inserted, a column added on one as the other writes, the other's table
# rewritten and an index made on it as the first writes.  (A wait for the other fails at once.)
$node->safe_psql('postgres', qq{SELECT test_branch_create('$_', 'main')}) for qw(a b);
my %id = map { $_ => $node->safe_psql('postgres', qq{SELECT test_branch_of('$_')}) } qw(a b);
my %s = map { $_ => $node->background_psql('postgres') } qw(a b);
$s{$_}->query_safe(qq{SELECT test_branch_checkout('$_'); SET ROLE app_owner; SET lock_timeout = '5s'; BEGIN}) for qw(a b);
$s{a}->query_safe(q{UPDATE app.t SET v = 'a' WHERE id <= 100});
$s{b}->query_safe(q{UPDATE app.t SET v = 'b' WHERE id <= 100});
$s{a}->query_safe(q{DELETE FROM app.t WHERE id BETWEEN 101 AND 150});
$s{b}->query_safe(q{DELETE FROM app.t WHERE id BETWEEN 141 AND 200});
$s{a}->query_safe(q{INSERT INTO app.t VALUES (5000, 'a')});
$s{b}->query_safe(q{INSERT INTO app.t VALUES (5000, 'b')});
$s{a}->query_safe(q{ALTER TABLE app.t ADD COLUMN extra integer DEFAULT 7});
$s{b}->query_safe(q{INSERT INTO app.t SELECT g, 'b' FROM generate_series(6000, 6999) g});
$s{b}->query_safe(q{ALTER TABLE app.t ALTER COLUMN v TYPE varchar(40)});
$s{a}->query_safe(q{UPDATE app.t SET extra = id WHERE id > 900});
$s{b}->query_safe(q{CREATE INDEX t_v_id ON app.t (v, id)});
$s{a}->query_safe(q{UPDATE app.t SET big = NULL WHERE id = 980});
is($s{a}->query_safe(q{SELECT count(*) FILTER (WHERE v = 'a') || '/' || count(*) FROM app.t}), '101/951',
	'each sees its own rows, not committed');
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
is(sql($id{a}, q{SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'app.t'::regclass AND attname = 'v'}),
	'text', 'nor the other\'s column type');
is(sql($id{a}, q{SELECT to_regclass('app.t_v_id') IS NULL}) . sql($id{b}, q{SELECT to_regclass('app.t_v_id') IS NOT NULL}),
	'tt', 'nor the other\'s index');
is(sql($id{a}, q{SELECT v FROM app.t WHERE id = 5000}) . sql($id{b}, q{SELECT v FROM app.t WHERE id = 5000}), 'ab',
	'each has its own row under the key both inserted');
is(sql($id{b}, q{SELECT big = repeat(md5('980'), 300) FROM app.t WHERE id = 980}), 't',
	'and the value stored out of line the other set aside');
for my $br (qw(a b))
{
	my $q = "SELECT count(*) FROM app.t WHERE v = '$br'";
	ok(by_index($node, $id{$br}, $q), "$br\'s rows are read through its index");
	is(sql($id{$br}, "SET enable_seqscan = off; $q"), $br eq 'a' ? '101' : '1101', 'and found there');
}
is(checks($id{a}), '3/0', 'every index and heap of one is right');
is(checks($id{b}), '4/0', 'and of the other');
is(rows($main), $c1_rows, 'and the branch they came from as it was');
is(checks($main), '3/0', 'its indexes and heap right');
for my $br (qw(a b))
{
	is(sql($id{$br}, q{SELECT test_capture_barrier('app')}), sql($id{$br}, q{SELECT test_ws_root_capture('app')}),
		"capture has $br\'s writes");
}
$s{$_}->quit for qw(a b);

# Two more, writing every page they share with each other and the one they came from, values stored
# out of line too, in statements running at the same time (both let go at once, from a lock held
# until both wait for it).
$node->safe_psql('postgres', qq{SELECT test_branch_create('$_', 'main')}) for qw(c d);
$id{$_} = $node->safe_psql('postgres', qq{SELECT test_branch_of('$_')}) for qw(c d);
$s{$_} = $node->background_psql('postgres') for qw(c d);
$s{$_}->query_safe(qq{SELECT test_branch_checkout('$_'); SET ROLE app_owner; BEGIN}) for qw(c d);
my $gate = $node->background_psql('postgres');
$gate->query_safe(q{SELECT pg_advisory_lock(147)});
for my $br (qw(c d))
{
	$s{$br}->query_until(qr/started/, qq{\\echo started
		SELECT pg_advisory_xact_lock_shared(147);
		SET LOCAL lock_timeout = '5s';
		UPDATE app.t SET v = v || '-$br';
		UPDATE app.t SET big = repeat(md5(id || '$br'), 300) WHERE id % 20 = 0 AND id % 40 <> 0;
		DELETE FROM app.t WHERE id % 40 = 0;
		INSERT INTO app.t SELECT g, '$br', repeat(md5(g || '$br'), 300) FROM generate_series(8000, 8199) g;
		UPDATE app.t SET v = v || '+';
		\\echo done
});
}
$node->poll_query_until('postgres', q{SELECT count(*) = 2 FROM pg_locks WHERE locktype = 'advisory' AND objid = 147 AND NOT granted})
  or die 'the writers did not wait';
$gate->query_safe(q{SELECT pg_advisory_unlock(147)});
$gate->quit;
$s{$_}->query_until(qr/done/, '') for qw(c d);
is($s{$_}->{stderr}, '', "$_: written at the same time as the other, without an error") for qw(c d);
$s{$_}->query_safe('COMMIT') for qw(c d);
for my $br (qw(c d))
{
	my $other = $br eq 'c' ? 'd' : 'c';
	is(sql($id{$br}, qq{SELECT count(*) || '/' || count(*) FILTER (WHERE v LIKE '%-$other%' OR v = '$other+')
		|| '/' || count(*) FILTER (WHERE big IS NOT NULL AND big <> repeat(md5(id || '$br'), 300)) FROM app.t}),
		'1175/0/0', "$br has its own rows and values stored out of line, none of the other's");
	is(checks($id{$br}), '3/0', "every index and heap of $br is right, TOAST too");
	is(sql($id{$br}, q{SELECT test_capture_barrier('app')}), sql($id{$br}, q{SELECT test_ws_root_capture('app')}),
		"capture has $br\'s writes");
	$s{$br}->quit;
}
is(rows($main), $c1_rows, 'the branch they came from as it was, its values stored out of line too');
is(checks($main), '3/0', 'its indexes and heap right');

# Changes not committed outlast the session that made them, a session leaving the branch and coming
# back, a restart and a crash.
$node->safe_psql('postgres', q{SELECT test_branch_make('p', 'main')});
my $w = $node->background_psql('postgres');
$w->query_safe(q{SELECT test_branch_checkout('p'); SET ROLE app_owner; UPDATE app.t SET v = 'p' WHERE id = 1;
	INSERT INTO app.t VALUES (7000, 'p'); RESET ROLE});
my $mine = q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t WHERE id IN (1, 7000)};
$w->query_safe(q{SELECT test_branch_checkout('main')});
is($w->query_safe($mine), '1:c1', 'a session that leaves a branch with changes not committed doesn\'t take them');
$w->query_safe(q{SELECT test_branch_checkout('p')});
is($w->query_safe($mine), '1:p,7000:p', 'and has them again as it comes back');
$w->quit;
my $next = q{SELECT test_branch_checkout('p'); } . $mine;
is((split /\n/, $node->safe_psql('postgres', $next))[-1], '1:p,7000:p',
	'changes not committed are seen by the next session on the branch');
$node->restart;
is((split /\n/, $node->safe_psql('postgres', $next))[-1], '1:p,7000:p', 'and after a restart');
$node->stop('immediate');
$node->start;
is((split /\n/, $node->safe_psql('postgres', $next))[-1], '1:p,7000:p', 'and after a crash');
is($node->safe_psql('postgres', q{SELECT test_branch_head('p')}), $c1, 'its head where it was');
is(rows($main), $c1_rows, 'and other branches without them');
my $p_rows = rows($node->safe_psql('postgres', q{SELECT test_branch_of('p')}));

# A standby follows each.
sub standby_of { return $standby->safe_psql('postgres', qq{SELECT test_branch_of('$_[0]')}); }
sub standby_rows { return $standby->safe_psql('postgres', "SET catalog_branch = $_[0]; $rows"); }
sub heads_match
{
	return $standby->safe_psql('postgres', qq{SELECT test_branch_head('$_[0]')})
	  eq $node->safe_psql('postgres', qq{SELECT test_branch_head('$_[0]')});
}
my %rows_of = (a => $a_rows, b => $b_rows, p => $p_rows, c => rows($id{c}), d => rows($id{d}));
$node->wait_for_replay_catchup($standby);
for my $br (qw(a b c d p))
{
	my $branch = $node->safe_psql('postgres', qq{SELECT test_branch_of('$br')});
	is(standby_of($br), $branch, "a standby has branch $br linked as its primary has");
	is(standby_rows($branch), $rows_of{$br}, "and reads its rows");
	ok(heads_match($br), "and its head");
}
for my $br (qw(a b))
{
	my $q = "SELECT count(*) FROM app.t WHERE v = '$br'";
	ok(by_index($standby, $id{$br}, $q), "$br\'s rows are read through its index on the standby");
	is($standby->safe_psql('postgres', "SET catalog_branch = $id{$br}; SET enable_seqscan = off; $q"), $br eq 'a' ? '101' : '1101',
		'and found there');
}
is($standby->safe_psql('postgres', "SET catalog_branch = $id{a}; SELECT sum(extra) FROM app.t"),
	$node->safe_psql('postgres', on($id{a}) . 'SELECT sum(extra) FROM app.t'), 'with its columns');

# Committed, given back, given one again, renamed and deleted: the standby follows.
sql($id{a}, q{SELECT test_branch_commit('a', 'a1')});
# (the sessions that were on them gone)
$node->poll_query_until('postgres', "SELECT NOT test_branch_in_use($id{a}) AND NOT test_branch_in_use($id{b})")
  or die 'the sessions on the branches did not leave';
$node->safe_psql('postgres', q{SELECT test_branch_dematerialize('a')});
$node->safe_psql('postgres', q{SELECT test_branch_rename('b', 'b2')});
$node->safe_psql('postgres', q{SELECT test_branch_delete('p', true)});
$node->wait_for_replay_catchup($standby);
isnt($node->safe_psql('postgres', q{SELECT test_branch_head('a')}), $c1, 'committed, a branch\'s head moves');
ok(heads_match('a'), 'and the standby has it there');
is($standby->safe_psql('postgres', q{SELECT test_branch_of('a') IS NULL}), 't', 'given back, a head alone');
is($standby->safe_psql('postgres', q{SELECT test_branch_of('b') IS NULL AND test_branch_of('b2') = } . $id{b}), 't',
	'one renamed by its new name');
ok(heads_match('b2'), 'its head with it');
is(standby_rows($id{b}), $b_rows, 'reading its rows');
is($standby->safe_psql('postgres', q{SELECT test_branch_of('p') IS NULL AND test_branch_head('p') IS NULL}), 't',
	'and one deleted, not at all');
my $a2 = $node->safe_psql('postgres', q{SELECT test_branch_materialize('a')});
$node->wait_for_replay_catchup($standby);
is(standby_of('a'), $a2, 'given one again, the standby has it');
is(standby_rows($a2), $a_rows, 'row for row');
is($standby->safe_psql('postgres', "SET catalog_branch = $a2; SELECT sum(extra) FROM app.t"),
	sql($a2, 'SELECT sum(extra) FROM app.t'), 'its columns too');

$standby->stop;
done_testing();
