# Copyright (c) 2026, PostgreSQL Global Development Group

# A rebase in progress survives a crash: its state is in the refs, with the
# transactions that made it.  One stopped at a conflict is still stopped
# after recovery, its plan and conflicts there, and goes on from there; a
# --continue whose transaction hadn't committed is undone, the rebase where
# it was before it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('rebase_crash');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# On the rebase's branch, as a session resuming it goes there.
sub on_rebase
{
	return $node->safe_psql('postgres',
		"SELECT dolt_checkout('dolt_rebase_b1') IS NOT NULL; $_[0]");
}

sql(q{
	CREATE SCHEMA rc VERSIONED;
	CREATE TABLE rc.t (pk int PRIMARY KEY, v text);
	SELECT dolt_commit('-Am', 'creating t');
	SELECT dolt_branch('b1');
	INSERT INTO rc.t VALUES (1, 'main');
	SELECT dolt_commit('-am', 'main row 1');
	SELECT dolt_checkout('b1');
	INSERT INTO rc.t VALUES (1, 'b1');
	SELECT dolt_commit('-am', 'b1 row 1');
	INSERT INTO rc.t VALUES (2, 'b1');
	SELECT dolt_commit('-am', 'b1 row 2');
	INSERT INTO rc.t VALUES (3, 'b1');
	SELECT dolt_commit('-am', 'b1 row 3');
});

# Stopped at the first step's conflict, kept.
my $stop = $node->safe_psql('postgres', q{
	SET dolt.allow_commit_conflicts = on;
	SELECT dolt_checkout('b1') IS NOT NULL;
	SELECT dolt_rebase('-i', 'main') IS NOT NULL;
	SELECT (dolt_rebase('--continue'))[1];
});
like($stop, qr/^t\nt\n1$/, 'the rebase stopped at a conflict');

$node->stop('immediate');
$node->start;

is(on_rebase(q{SELECT rebase_order, action, commit_message FROM dolt_rebase ORDER BY 1}),
	"t\n1.00|pick|b1 row 1\n2.00|pick|b1 row 2\n3.00|pick|b1 row 3",
	'after a crash, the plan is there');
is(on_rebase(q{SELECT table_name, num_conflicts FROM dolt_conflicts}),
	"t\nrc.t|1", 'and the conflict it stopped at');

# Resolved, then continued in a transaction that never commits.
$node->safe_psql('postgres', q{
	SET dolt.allow_commit_conflicts = on;
	SELECT dolt_checkout('dolt_rebase_b1');
	SELECT dolt_conflicts_resolve('--theirs', 'rc.t');
});
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT dolt_checkout('dolt_rebase_b1')});
$s->query_safe(q{BEGIN});
like($s->query_safe(q{SELECT dolt_rebase('--continue')}),
	qr/Successfully rebased/, 'a --continue to the end, not committed');
$node->stop('immediate');
$s->{run}->finish;
$node->start;

is(on_rebase(q{SELECT count(*) FROM dolt_rebase}), "t\n3",
	'after a crash, the uncommitted --continue is undone');
is(on_rebase(q{SELECT pk, v FROM rc.t ORDER BY pk}), "t\n1|b1",
	'the resolution kept');
like(on_rebase(q{SELECT dolt_rebase('--continue')}), qr/Successfully rebased and updated refs\/heads\/b1/,
	'and the rebase goes on from there');
is(sql(q{SELECT dolt_checkout('b1') IS NOT NULL; SELECT pk, v FROM rc.t ORDER BY pk}),
	"t\n1|b1\n2|b1\n3|b1", 'the rebased branch has its rows');
is(sql(q{SELECT dolt_checkout('b1') IS NOT NULL; SELECT message FROM dolt_log LIMIT 4}),
	"t\nb1 row 3\nb1 row 2\nb1 row 1\nmain row 1", 'and its commits, onto main');
is(sql(q{SELECT count(*) FROM dolt_branches WHERE name = 'dolt_rebase_b1'}), '0',
	"the rebase's branch is gone");

$node->stop;
done_testing();
