# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit, as Dolt's: of the branch's working set as of one point, made by its author and committer
# (the session's user and user@host unless given, the committer the author's unless given, dates the
# commit's time unless given, before 1970 too).  Nothing to commit is refused, but for --allow-empty;
# --skip-empty makes none; amending takes the head's place, parents and message.  Dolt's refusals:
# both empties, no message, an empty name or email.  A branch with no working set commits its head.
# Called outside a transaction block, the SQL transaction is committed first and the commit made in
# one of its own, durable when the call returns; in a transaction block, it's made in the transaction,
# undone with it, and refused once that has written.  Two sessions committing one branch both land.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vc_commit');
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
sub head { return $node->safe_psql('postgres', qq{SELECT test_branch_head('$_[0]')}); }
sub meta
{
	my ($commit, $cols) = @_;
	return $node->safe_psql('postgres', qq{SELECT $cols FROM test_vc_commit_meta('$commit')});
}
sub refused
{
	my ($sql, $want, $name) = @_;
	my ($rc, $out, $err) = $node->psql('postgres', $sql);
	like($err, $want, $name);
}

sql($main, q{CREATE SCHEMA app VERSIONED; CREATE TABLE app.t (id integer PRIMARY KEY, v text);
	INSERT INTO app.t SELECT g, 'c1' FROM generate_series(1, 5) g});
sql($main, "SELECT test_branch_link('main', $main)");
my $init = head('main');

# Made by the session's user, and the commit's time, as Dolt's is unless told.
my $c1 = sql($main, q{SELECT test_vc_ops_commit('main', 'c1')});
is(head('main'), $c1, 'a commit moves the head');
# (the client's address, a socket's this host)
my ($user, $email) = split /\|/, $node->safe_psql('postgres',
	q{SELECT session_user, session_user || '@' || coalesce(host(inet_client_addr()), 'localhost')});
is(meta($c1, q{author || ' ' || author_email || ' ' || committer || ' ' || committer_email || ' ' || message}),
	"$user $email $user $email c1", 'by the session\'s user and user@host, author and committer');
is(meta($c1, q{author_date = committer_date AND author_date BETWEEN now() - interval '1 hour' AND now()}), 't',
	'dated now, both');
is(meta($c1, q{parents[1]}), $init, 'after the head');
is(meta($c1, q{test_dbroot_schemas(root)}),
	'app=' . sql($main, q{SELECT test_ws_root_capture('app')}), 'of the working set');

# Nothing to commit.
refused(on($main) . q{SELECT test_vc_ops_commit('main', 'again')}, qr/nothing to commit/,
	'nothing to commit is refused');
is(sql($main, q{SELECT test_vc_ops_commit('main', 'again', skip_empty => true) IS NULL}), 't',
	'with --skip-empty, no commit and no error');
is(head('main'), $c1, 'and the head stays');
my $e = sql($main, q{SELECT test_vc_ops_commit('main', 'empty', allow_empty => true)});
is(meta($e, q{parents[1]}), $c1, 'with --allow-empty, a commit after the head');
is(meta($e, 'root'), meta($c1, 'root'), 'of its root');

# Dolt's refusals.
refused(on($main) . q{SELECT test_vc_ops_commit('main', 'x', allow_empty => true, skip_empty => true)},
	qr/error: cannot use both --allow-empty and --skip-empty/, 'both empties refused');
refused(on($main) . q{SELECT test_vc_ops_commit('main')}, qr/Must provide commit message\./, 'no message refused');
refused(on($main) . q{SELECT test_vc_ops_commit('main', '', allow_empty => true)},
	qr/Aborting commit due to empty commit message\./, 'an empty message refused');
refused(on($main) . q{SELECT test_vc_ops_commit('main', 'x', author => '', allow_empty => true)},
	qr/Aborting commit due to empty author name\. Is your config set\?/, 'an empty author refused');
refused(on($main) . q{SELECT test_vc_ops_commit('main', 'x', committer_email => '', allow_empty => true)},
	qr/Aborting commit due to empty committer email\. Is your config set\?/, 'an empty committer email refused');
refused(on($main) . q{SELECT test_vc_ops_commit('nowhere', 'x', allow_empty => true)},
	qr/branch "nowhere" not found/, 'a branch that isn\'t refused');
is(head('main'), $e, 'none of them moves the head');

# Who, as given: --author and --date set the committer's too, unless it's given.
sql($main, q{UPDATE app.t SET v = 'c2' WHERE id = 1});
my $c2 = sql($main, q{SELECT test_vc_ops_commit('main', 'c2', author => 'Ann', author_email => 'ann@x',
	author_date => '1960-01-02 03:04:05.678901+00')});
is(meta($c2, q{author || ' ' || author_email || ' ' || committer || ' ' || committer_email}),
	'Ann ann@x Ann ann@x', 'the author given, the committer the author');
is(meta($c2, q{author_date = '1960-01-02 03:04:05.678901+00' AND committer_date = author_date}), 't',
	'dated as given, before 1970, to the microsecond, both');
sql($main, q{UPDATE app.t SET v = 'c3' WHERE id = 2});
my $c3 = sql($main, q{SELECT test_vc_ops_commit('main', 'c3', author => 'Ann', author_email => 'ann@x',
	committer => 'Cy', committer_email => 'cy@x', committer_date => '2030-01-01+00')});
is(meta($c3, q{author || ' ' || committer || ' ' || committer_email || ' ' || (committer_date = '2030-01-01+00')
	|| ' ' || (author_date < committer_date)}), 'Ann Cy cy@x true true', 'the committer given apart');

# Amended: the head's place, parents and message, with what changed since; empty too.
sql($main, q{UPDATE app.t SET v = 'c3b' WHERE id = 3});
my $a = sql($main, q{SELECT test_vc_ops_commit('main', amend => true)});
is(head('main'), $a, 'an amend moves the head');
is(meta($a, q{message || ' ' || parents[1] || ' ' || cardinality(parents)}), "c3 $c2 1",
	'in the amended one\'s place, with its message and parents');
is(meta($a, q{test_dbroot_schemas(root)}),
	'app=' . sql($main, q{SELECT test_ws_root_capture('app')}), 'of the working set now');
my $a2 = sql($main, q{SELECT test_vc_ops_commit('main', 'reworded', amend => true)});
is(meta($a2, q{message || ' ' || parents[1]}), "reworded $c2", 'amended with nothing changed, a message given');
is(meta($a2, 'root'), meta($a, 'root'), 'the same root');
is(sql($main, q{SELECT test_vc_ops_commit('main', 'x', amend => true, skip_empty => true) IS NULL}), 't',
	'amending with --skip-empty and nothing changed makes none, as Dolt\'s');
is(head('main'), $a2, 'the head kept');

# A branch with no working set: its head is one.
$node->safe_psql('postgres', q{SELECT test_branch_make('h', 'main')});
refused(q{SELECT test_vc_ops_commit('h', 'x')}, qr/nothing to commit/,
	'a branch with no working set has nothing to commit');
my $h = $node->safe_psql('postgres', q{SELECT test_vc_ops_commit('h', 'on h', allow_empty => true)});
is(meta($h, q{parents[1]}), $a2, 'allowed empty, it\'s committed after its head');
is(meta($h, 'root'), meta($a2, 'root'), 'of its head\'s root');
is($node->safe_psql('postgres', q{SELECT test_branch_of('h') IS NULL}), 't', 'and given no working set');
my $ha = $node->safe_psql('postgres', q{SELECT test_vc_ops_commit('h', 'on h, amended', amend => true)});
is(meta($ha, q{message || ' ' || parents[1] || ' ' || (root = '} . meta($h, 'root') . q{')}), "on h, amended $a2 true",
	'amended, in its head\'s place');

# In a transaction block: made in it, undone with it, refused once it has written.
refused(on($main) . q{BEGIN; INSERT INTO app.t VALUES (10, 'w'); SELECT test_vc_ops_commit('main', 'w')},
	qr/a commit can't be made in a transaction that has written/, 'refused in a transaction that has written');
refused(on($main) . q{BEGIN; INSERT INTO app.t VALUES (10, 'w'); CALL test_vc_ops_commit_call('main', 'w')},
	qr/a commit can't be made in a transaction that has written/, 'called in one too');
sql($main, q{INSERT INTO app.t VALUES (10, 'w')});
sql($main, q{BEGIN; SELECT test_vc_ops_commit('main', 'rolled back'); ROLLBACK});
is(head('main'), $a2, 'made in a transaction block, it\'s undone with it');
sql($main, q{BEGIN; CALL test_vc_ops_commit_call('main', 'in a block'); COMMIT});
my $b = head('main');
isnt($b, $a2, 'called in a transaction block, it\'s made in it');
is(meta($b, q{message}), 'in a block', 'as told');
refused(on($main) . q{BEGIN; SELECT test_vc_ops_commit('main', 'one', allow_empty => true);
	SELECT test_vc_ops_commit('main', 'two', allow_empty => true)},
	qr/a commit can't be made in a transaction that has written\nDETAIL:  A commit writes, so a transaction block can make only one\./,
	'a second commit in a transaction block refused, as the first wrote');
is(head('main'), $b, 'and the first with it');

# Called outside one: the transaction committed first, its writes then in the commit, which is
# durable when the call returns.
sql($main, q{DO $$BEGIN UPDATE app.t SET v = 'called' WHERE id = 4; CALL test_vc_ops_commit_call('main', 'called');
	END$$});
my $called = head('main');
is(meta($called, q{message || ' ' || parents[1]}), "called $b", 'called outside a transaction block');
is(meta($called, q{test_dbroot_schemas(root)}),
	'app=' . sql($main, q{SELECT test_ws_root_capture('app')}), 'with what the transaction had written');
refused(on($main) . q{DO $$BEGIN UPDATE app.t SET v = 'kept' WHERE id = 5;
	CALL test_vc_ops_commit_call('main', 'kept'); RAISE EXCEPTION 'after it'; END$$}, qr/after it/,
	'an error after the call');
is(meta(head('main'), q{message}), 'kept', 'leaves the commit made');
is(sql($main, q{SELECT v FROM app.t WHERE id = 5}), 'kept', 'and the writes before it');
is($node->safe_psql('postgres', on($main) . q{CALL test_vc_ops_commit_call('main', 'skipped', skip_empty => true);
	SELECT test_branch_head('main')}), head('main'), 'skipped empty, called, nothing made');

# Two sessions committing one branch at once both land, the second after the first.
my $before = head('main');
my $s1 = $node->background_psql('postgres');
my $s2 = $node->background_psql('postgres');
$s1->query_safe(on($main) . q{BEGIN; SELECT test_vc_ops_commit('main', 'first', allow_empty => true)});
$s2->query_until(qr/started/, on($main) . q{\echo started
	SELECT test_vc_ops_commit('main', 'second', allow_empty => true);
	\echo done
});
$node->poll_query_until('postgres',
	q{SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE '%''second''%'})
  or die 'the second commit never waited';
$s1->query_safe('COMMIT');
$s2->query_until(qr/done/, '');
my $second = head('main');
my $first = meta($second, q{parents[1]});
is(meta($first, q{message || ' ' || parents[1]}), "first $before", 'the first after the head');
is(meta($second, q{message}), 'second', 'the second after the first, waiting its turn');

# A branch renamed as its commit waited for it: the commit refused, as it would be of a name no longer
# the branch's.
$node->safe_psql('postgres', q{SELECT test_branch_create('f', 'main')});
# (the waiting session carrying on past its error, to say it's done)
my $s3 = $node->background_psql('postgres', on_error_stop => 0);
$s1->query_safe(q{BEGIN; SELECT test_branch_rename('f', 'g')});
$s3->query_until(qr/started/, q{\echo started
	SELECT test_vc_ops_commit('f', 'renamed away', allow_empty => true);
	\echo done
});
$node->poll_query_until('postgres',
	q{SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE '%renamed away%'})
  or die 'the commit never waited for the rename';
$s1->query_safe('COMMIT');
$s3->query_until(qr/done/, '');
like($s3->{stderr}, qr/branch "f" changed as it was being committed/, 'a commit waiting out a rename is refused');
is(head('g'), $second, 'and made nothing on it');
$s1->quit;
$s2->quit;
$s3->quit;

done_testing();
