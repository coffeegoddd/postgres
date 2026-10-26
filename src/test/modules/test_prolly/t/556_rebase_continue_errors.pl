# Copyright (c) 2026, PostgreSQL Global Development Group

# dolt_rebase('--continue') runs its steps in a subtransaction, to abort the
# rebase where a step fails as Dolt aborts it (a schema conflict, conflicts
# the session can't keep).  Any other error there, a cancel or one raised
# in a step, fails the statement instead: the rebase is left as it was
# before it, on its branch with its plan, to continue once more.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}
my $node = PostgreSQL::Test::Cluster->new('rebase_errors');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	SELECT dolt_commit('-Am', 'base');
	SELECT dolt_branch('b');
	INSERT INTO s.t VALUES (0, 'main');
	SELECT dolt_commit('-am', 'main row 0');
	SELECT dolt_checkout('b');
	INSERT INTO s.t VALUES (1, 'b');
	SELECT dolt_commit('-am', 'b row 1');
	INSERT INTO s.t VALUES (2, 'b');
	SELECT dolt_commit('-am', 'b row 2');
});

my $s = $node->background_psql('postgres', on_error_stop => 0);
my $pid = $s->query_safe('SELECT pg_backend_pid()');
$s->query_safe(q{SELECT dolt_checkout('b'); SELECT dolt_rebase('-i', 'main')});
my $onto = $s->query_safe(q{SELECT dolt_hashof('HEAD')});

sub left_as_it_was
{
	my ($what) = @_;
	is($s->query_safe(q{SELECT active_branch(), (SELECT count(*) FROM dolt_rebase)}),
		'dolt_rebase_b|2', "$what: the rebase is still in progress, its plan kept");
	is($s->query_safe(q{SELECT dolt_hashof('HEAD')}), $onto,
		"$what: no step's pick is kept");
}

# A cancel in a step.
$s->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-rebase-step', 'wait')});
$s->query_until(qr/continuing/, q{\echo continuing
	SELECT dolt_rebase('--continue');
});
$node->wait_for_event('client backend', 'prolly-rebase-step');
sql("SELECT pg_cancel_backend($pid)");
$s->query('SELECT 1');
like($s->{stderr}, qr/canceling statement due to user request/,
	'the --continue was canceled');
$s->{stderr} = '';
left_as_it_was('canceled');
$s->query_safe(q{SELECT injection_points_detach('prolly-rebase-step')});

# An error raised in a step, in a transaction block: rethrown, the block
# aborted, and the rebase as it was once it is rolled back.
$s->query_safe(q{SELECT injection_points_attach('prolly-rebase-step', 'error')});
$s->query(q{BEGIN; SELECT dolt_rebase('--continue')});
like($s->{stderr}, qr/error triggered for injection point prolly-rebase-step/,
	'an error in a step fails the --continue');
$s->{stderr} = '';
$s->query_safe('ROLLBACK');
left_as_it_was('failed');
$s->query_safe(q{SELECT injection_points_detach('prolly-rebase-step')});

like($s->query_safe(q{SELECT dolt_rebase('--continue')}),
	qr/Successfully rebased and updated refs\/heads\/b/,
	'the rebase continues once more, to its end');
is($s->query_safe(q{SELECT active_branch(), (SELECT count(*) FROM s.t)}), 'b|3',
	'the session is on the branch rebased, with every row');
$s->quit;

$node->stop;
done_testing();
