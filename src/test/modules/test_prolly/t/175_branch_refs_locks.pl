# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit holds its own branch's refs, not every ref: commits on different
# branches don't wait for each other, and each keeps the others' commits;
# commits on one branch, and whatever changes other refs, still take turns.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_refs_locks');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
max_prepared_transactions = 2
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	INSERT INTO app.t VALUES (1, 'a');
	SELECT dolt_commit('-am', 'first');
	SELECT dolt_branch('f');
	SELECT dolt_branch('g');
});

sub waits_for
{
	my ($what) = @_;
	return $node->poll_query_until('postgres', qq{
		SELECT count(*) > 0 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		WHERE NOT l.granted AND a.query LIKE '%$what%'});
}

sub log_of
{
	my ($branch) = @_;
	return sql(qq{SELECT string_agg(message, ',') FROM dolt_log('$branch')});
}

# Each branch's head names the rows its working set has.
sub roots_agree
{
	my ($s, $what) = @_;
	is($s->query_safe(q{SELECT test_fold_root() = prolly_root_hash(active_branch())}),
		't', $what);
}

my $a = $node->background_psql('postgres', on_error_stop => 0);
my $b = $node->background_psql('postgres', on_error_stop => 0);
my $c = $node->background_psql('postgres', on_error_stop => 0);
$b->query_safe(q{SELECT dolt_checkout('f')});
$c->query_safe(q{SELECT dolt_checkout('g')});
$b->query_safe(q{SET lock_timeout = '5s'});
$c->query_safe(q{SET lock_timeout = '5s'});

# Three branches committing at once: none waits for another's open
# transaction, and every commit stays.
$a->query_safe(q{BEGIN; INSERT INTO app.t VALUES (2, 'main');
	SELECT dolt_commit('-am', 'on main')});
$b->query_safe(q{BEGIN; INSERT INTO app.t VALUES (2, 'f');
	SELECT dolt_commit('-am', 'on f')});
$c->query_safe(q{INSERT INTO app.t VALUES (2, 'g');
	SELECT dolt_commit('-am', 'on g')});
is($c->{stderr}, '', 'a commit on g waits for neither open commit');
is(log_of('g'), 'on g,first', 'g has its commit at once');

# What a holder reads of the refs is the others' commits and its own.
is($a->query_safe(q{SELECT string_agg(message, ',') FROM dolt_log('g')}),
	'on g,first', 'a holder reads another branch\'s commit made since');
is($a->query_safe(q{SELECT string_agg(message, ',') FROM dolt_log('main')}),
	'on main,first', 'and its own');
$b->query_safe('COMMIT');
$a->query_safe('COMMIT');
is(log_of('main'), 'on main,first', 'main has its commit');
is(log_of('f'), 'on f,first', 'f its own');
is(log_of('g'), 'on g,first', 'g its own');
roots_agree($a, 'main\'s head holds its rows');
roots_agree($b, 'f\'s too');
roots_agree($c, 'g\'s too');

# A savepoint rolled back takes its commit with it, while another branch's
# commit lands meanwhile.
$a->query_safe(q{BEGIN; SAVEPOINT s; INSERT INTO app.t VALUES (3, 'gone');
	SELECT dolt_commit('-am', 'rolled back')});
$c->query_safe(q{INSERT INTO app.t VALUES (3, 'g');
	SELECT dolt_commit('-am', 'g again')});
$a->query_safe(q{ROLLBACK TO s; INSERT INTO app.t VALUES (3, 'kept');
	SELECT dolt_commit('-am', 'kept'); COMMIT});
is(log_of('main'), 'kept,on main,first', 'the rolled-back commit is gone');
is(log_of('g'), 'g again,on g,first', 'and g\'s commit made meanwhile stays');
roots_agree($a, 'main\'s head holds the kept row');

# Two committers on one branch still take turns: the second builds on the
# first's commit, with its rows.
my $d = $node->background_psql('postgres', on_error_stop => 0);
$d->query_safe(q{SELECT dolt_checkout('f')});
$b->query_safe(q{BEGIN; INSERT INTO app.t VALUES (4, 'first of two');
	SELECT dolt_commit('-am', 'first of two')});
$d->query_until(qr/started/, q{
	\echo started
	INSERT INTO app.t VALUES (5, 'second of two');
	SELECT dolt_commit('-am', 'second of two');
	\echo done
});
ok(waits_for('second of two'), 'a second committer on f waits for the first');
$b->query_safe('COMMIT');
$d->query_until(qr/done/, '');
is($d->{stderr}, '', 'and then commits');
is(log_of('f'), 'second of two,first of two,on f,first', 'on top of the first');
roots_agree($d, 'with the first one\'s rows');

# A change of other refs (a branch made from main) doesn't wait for main's
# open commit either: it reads main's head as last committed.
$a->query_safe(q{BEGIN; INSERT INTO app.t VALUES (6, 'main');
	SELECT dolt_commit('-am', 'before the branch')});
my $main_before = log_of('main');
$d->query_safe(q{SET lock_timeout = '5s'});
$d->query_safe(q{SELECT dolt_branch('h', 'main')});
is($d->{stderr}, '', 'a branch creation doesn\'t wait for a commit');
is(log_of('h'), $main_before, 'and is made at the head committed before it');
$a->query_safe('COMMIT');
like(log_of('main'), qr/^before the branch,/, 'main has its commit');
$d->query_safe(q{RESET lock_timeout});

# A commit waits for an open change of other refs, to move the root.
$d->query_safe(q{BEGIN; SELECT dolt_branch('i')});
$c->query_safe(q{RESET lock_timeout});
$c->query_until(qr/started/, q{
	\echo started
	INSERT INTO app.t VALUES (7, 'g');
	SELECT dolt_commit('-am', 'after the branch');
	\echo done
});
ok(waits_for('after the branch'), 'a commit waits for a branch creation');
$d->query_safe('COMMIT');
$c->query_until(qr/done/, '');
is($c->{stderr}, '', 'and then commits');
is(log_of('g'), 'after the branch,g again,on g,first', 'g has it');
ok(sql(q{SELECT count(*) FROM dolt_branches WHERE name = 'i'}) == 1,
	'and the branch is there');

# A committer that comes to change other refs moves the root from then on:
# its branch is made at its own commit, and another branch's open commit
# is kept.
$b->query_safe(q{BEGIN; INSERT INTO app.t VALUES (8, 'f');
	SELECT dolt_commit('-am', 'f open')});
$a->query_safe(q{BEGIN; INSERT INTO app.t VALUES (8, 'main');
	SELECT dolt_commit('-am', 'main then branch'); SELECT dolt_branch('j')});
$a->query_safe('COMMIT');
$b->query_safe('COMMIT');
is($a->{stderr}, '', 'the branch is made');
is(log_of('j'), log_of('main'), 'at its maker\'s own commit');
like(log_of('main'), qr/^main then branch,/, 'main has the commit');
like(log_of('f'), qr/^f open,/, 'f keeps its own');
roots_agree($b, 'f\'s head holds its rows');

# A branch deleted while another session commits on it waits for that
# commit, and the commit isn't brought back.
$c->query_safe(q{SELECT dolt_checkout('i')});
$c->query_safe(q{BEGIN; INSERT INTO app.t VALUES (11, 'i');
	SELECT dolt_commit('-am', 'on i'); SELECT dolt_checkout('g')});
$d->query_until(qr/started/, q{
	\echo started
	SELECT dolt_branch('-D', 'i');
	\echo done
});
ok(waits_for('dolt_branch'), 'a deletion waits for the branch\'s open commit');
$c->query_safe('COMMIT');
$d->query_until(qr/done/, '');
is($d->{stderr}, '', 'and then deletes it');
is(sql(q{SELECT count(*) FROM dolt_branches WHERE name = 'i'}), '0',
	'the branch stays deleted');

# dolt.transaction_commit's commit, made as the transaction commits,
# doesn't wait for another branch's open commit either.
$a->query_safe(q{BEGIN; INSERT INTO app.t VALUES (9, 'main');
	SELECT dolt_commit('-am', 'main open')});
$b->query_safe(q{SET dolt.transaction_commit = on; SET lock_timeout = '5s'});
$b->query_safe(q{INSERT INTO app.t VALUES (9, 'f')});
$b->query_safe(q{BEGIN; UPDATE app.t SET v = 'f2' WHERE id = 9; COMMIT});
is($b->{stderr}, '', 'transaction commits on f wait for no commit on main');
like(log_of('f'), qr/^Transaction commit,Transaction commit,f open,/,
	'each made its commit');
$a->query_safe('COMMIT');
like(log_of('main'), qr/^main open,/, 'and main has its own');
$b->query_safe(q{RESET dolt.transaction_commit});
roots_agree($a, 'main\'s head holds its rows');
roots_agree($b, 'f\'s too');

# A collection while a commit is open, after another branch's commit
# moved the root past the refs that commit built on: the open commit's
# changes still name chunks of that root, which the collection keeps, and
# it commits.  (Enough branches that the refs tree has inner nodes, which
# the open commit's tree shares with the root it built on.)
sql(q{SELECT count(dolt_branch('many' || lpad(g::text, 4, '0')))
	FROM generate_series(1, 600) g; SELECT dolt_branch('zzz')});
$c->query_safe(q{SELECT dolt_checkout('zzz')});
$a->query_safe(q{BEGIN; INSERT INTO app.t VALUES (12, 'main');
	SELECT dolt_commit('-am', 'open across a collection')});
$c->query_safe(q{INSERT INTO app.t VALUES (12, 'zzz');
	SELECT dolt_commit('-am', 'zzz before the collection')});
$d->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_gc(true);
	\echo collected
});
ok($node->poll_query_until('postgres', q{
	SELECT count(*) > 0 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
	WHERE NOT l.granted AND a.query LIKE '%test_chunk_gc%'}),
	'the collection waits for the open commit');
$a->query(q{COMMIT});
is($a->{stderr}, '', 'a commit open across a collection commits');
$a->{stderr} = '';
$d->query_until(qr/collected/, '');
like(log_of('main'), qr/^open across a collection,/, 'main has it');
like(log_of('zzz'), qr/^zzz before the collection,/, 'zzz keeps its own');
$c->query_safe(q{SELECT dolt_checkout('g')});
roots_agree($a, 'main\'s head holds its rows after the collection');

# A committer that escalates (makes a branch) in a savepoint rolled back:
# its commit goes back to holding its branch alone, another branch's
# commit doesn't wait for it, both are kept, and the branch is gone.
$a->query_safe(q{BEGIN; INSERT INTO app.t VALUES (13, 'main');
	SELECT dolt_commit('-am', 'kept past an escalation');
	SAVEPOINT s; SELECT dolt_branch('escalated'); ROLLBACK TO s;
	SELECT dolt_commit('--allow-empty', '-m', 'again past an escalation')});
$c->query_safe(q{SET lock_timeout = '5s'});
$c->query_safe(q{INSERT INTO app.t VALUES (13, 'g');
	SELECT dolt_commit('-am', 'g past an escalation')});
is($c->{stderr}, '', 'another branch\'s commit doesn\'t wait for it');
$a->query_safe(q{COMMIT});
like(log_of('main'), qr/^again past an escalation,kept past an escalation,/,
	'the commits are kept');
like(log_of('g'), qr/^g past an escalation,/, 'and the other branch\'s');
is(sql(q{SELECT count(*) FROM dolt_branches WHERE name = 'escalated'}), '0',
	'the branch made in the savepoint is gone');

# The same, the escalation in an inner savepoint released into one rolled
# back.
$a->query_safe(q{BEGIN; INSERT INTO app.t VALUES (14, 'main');
	SELECT dolt_commit('-am', 'kept past a nested escalation');
	SAVEPOINT s1; SAVEPOINT s2; SELECT dolt_branch('escalated2');
	RELEASE s2; ROLLBACK TO s1;
	SELECT dolt_commit('--allow-empty', '-m', 'again past a nested escalation')});
$c->query_safe(q{INSERT INTO app.t VALUES (14, 'g');
	SELECT dolt_commit('-am', 'g past a nested escalation')});
is($c->{stderr}, '', 'nor then');
$a->query_safe(q{COMMIT});
like(log_of('main'), qr/^again past a nested escalation,kept past a nested escalation,/,
	'the commits are kept');
like(log_of('g'), qr/^g past a nested escalation,/, 'and the other\'s');
is(sql(q{SELECT count(*) FROM dolt_branches WHERE name = 'escalated2'}), '0',
	'and the branch is gone');
$c->query_safe(q{RESET lock_timeout});
roots_agree($a, 'main\'s head holds its rows');
roots_agree($c, 'g\'s too');

# A transaction that committed can't be prepared, as before.
$a->query(q{BEGIN; INSERT INTO app.t VALUES (10, 'prepared');
	SELECT dolt_commit('-am', 'prepared'); PREPARE TRANSACTION 'p'});
like($a->{stderr}, qr/cannot prepare a transaction that moved the chunk store's root/,
	'a commit can\'t be prepared');
$a->{stderr} = '';
unlike(log_of('main'), qr/prepared/, 'and isn\'t made');

$_->quit for ($a, $b, $c, $d);
$node->stop;
done_testing();
