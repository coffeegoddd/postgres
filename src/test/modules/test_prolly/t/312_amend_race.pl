# Copyright (c) 2026, PostgreSQL Global Development Group

# An amend, or a rebase's reword, replaces commits from the head it read:
# a commit another session makes on the branch meanwhile must not be
# dropped.  The head is read again once the branch is held; moved, the
# amend (or the --continue) fails to be retried, and the other's commit
# stays.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('amend_race');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Wait till a session's statement waits for a lock.
sub wait_locked
{
	my ($pid) = @_;
	$node->poll_query_until('postgres', qq{SELECT wait_event_type = 'Lock'
		FROM pg_stat_activity WHERE pid = $pid}) or die "$pid did not wait";
}

sql(q{
	CREATE SCHEMA ar VERSIONED;
	CREATE TABLE ar.t (pk int PRIMARY KEY, v text);
	SELECT dolt_commit('-Am', 'c0');
	INSERT INTO ar.t VALUES (1, 'one');
	SELECT dolt_commit('-am', 'c1');
});

# B commits c2 and holds the branch; A amends meanwhile.
my $b = $node->background_psql('postgres');
$b->query_safe(q{BEGIN});
$b->query_safe(q{INSERT INTO ar.t VALUES (2, 'two')});
$b->query_safe(q{SELECT dolt_commit('-am', 'c2 by B')});
my $a = $node->background_psql('postgres', on_error_stop => 0);
my $apid = $a->query_safe(q{SELECT pg_backend_pid()});
$a->query_until(qr/started/, q{
	\\echo started
	SELECT dolt_commit('--amend', '-m', 'c1 amended');
	\\echo done
});
wait_locked($apid);
$b->query_safe(q{COMMIT});
$a->query_until(qr/done/, '');
like($a->{stderr}, qr/could not serialize access/, 'the amend fails, the branch moved');
is(sql(q{SELECT string_agg(message, ',') FROM dolt_log}), 'c2 by B,c1,c0',
	"the other's commit stays");
is(sql(q{SELECT count(*) FROM ar.t}), '2', 'with its row');
$a->quit;
$b->quit;

# A rebase whose first step rewords its pick: B commits on the rebase's
# branch as A goes on.
sql(q{
	SELECT dolt_branch('b1');
	INSERT INTO ar.t VALUES (3, 'main');
	SELECT dolt_commit('-am', 'main row 3');
	SELECT dolt_checkout('b1');
	INSERT INTO ar.t VALUES (10, 'b1');
	SELECT dolt_commit('-am', 'b1 row 10');
	INSERT INTO ar.t VALUES (20, 'b1');
	SELECT dolt_commit('-am', 'b1 row 20');
});
$a = $node->background_psql('postgres', on_error_stop => 0);
$apid = $a->query_safe(q{SELECT pg_backend_pid()});
$a->query_safe(q{SELECT dolt_checkout('b1')});
$a->query_safe(q{SELECT dolt_rebase('-i', 'main')});
$a->query_safe(q{UPDATE dolt_rebase SET action = 'reword', commit_message = 'reworded'
	WHERE rebase_order = 1});
$b = $node->background_psql('postgres');
$b->query_safe(q{SELECT dolt_checkout('dolt_rebase_b1')});
$b->query_safe(q{BEGIN});
$b->query_safe(q{INSERT INTO ar.t VALUES (99, 'by B')});
$b->query_safe(q{SELECT dolt_commit('-am', 'B on the rebase branch')});
$a->query_until(qr/started/, q{
	\\echo started
	SELECT dolt_rebase('--continue');
	\\echo done
});
wait_locked($apid);
$b->query_safe(q{COMMIT});
$a->query_until(qr/done/, '');
like($a->{stderr}, qr/could not serialize access/, '--continue fails, the branch moved');
$b->quit;		# (the rebase's branch, deleted as it ends, is B's no longer)
$a->{stderr} = '';
like($a->query_safe(q{SELECT dolt_rebase('--continue')}), qr/Successfully rebased/,
	'and goes on when retried');
is($a->query_safe(q{SELECT string_agg(pk::text, ',' ORDER BY pk) FROM ar.t}), '1,2,3,10,20,99',
	"the other's row is on the rebased branch");
is($a->query_safe(q{SELECT string_agg(message, ',') FROM (SELECT message FROM dolt_log LIMIT 4) m}),
	'b1 row 20,reworded,B on the rebase branch,main row 3', "and its commit");
$a->quit;

$node->stop;
done_testing();
