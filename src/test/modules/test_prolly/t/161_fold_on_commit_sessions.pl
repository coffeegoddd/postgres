# Copyright (c) 2026, PostgreSQL Global Development Group

# prolly.fold_on_commit across sessions.  A writer's commit folds its own
# changes only if the refs are free: one that another transaction holds
# (a dolt_commit in it, not yet committed) is left to the next fold, the
# writer's COMMIT neither waiting nor deadlocking.  And what other
# writers left unfolded stays to be folded: a lazy writer's change before
# a fold-on writer's of the same row, a lazy writer in flight across
# fold-on commits (keyed and keyless tables), and a repeatable-read
# fold-on writer whose snapshot is older than another's fold.  Each time
# status and HEAD..WORKING are as a fold in full has them, and the commit
# that follows holds the same.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fold_on_commit_sessions');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA c VERSIONED;
	CREATE TABLE c.t (id int PRIMARY KEY, v text);
	CREATE TABLE c.k (v text);
	INSERT INTO c.t SELECT i, 'v' || i FROM generate_series(1, 100) i;
	INSERT INTO c.k SELECT 'k' || i FROM generate_series(1, 10) i;
	SELECT dolt_commit('-Am', 'base')});

# What a fold has the working set as, and what a fold in full has it as.
my $seen = q{
	SELECT coalesce((SELECT string_agg(table_name || ' ' || status, ',' ORDER BY 1)
			FROM dolt_status), '') || ' | ' ||
		coalesce((SELECT string_agg(diff_type || ':' || coalesce((to_row).id, (from_row).id)
				|| ':' || coalesce((to_row).v, '-'), ',' ORDER BY 1)
			FROM dolt_diff('HEAD', 'WORKING', NULL::c.t)), '') || ' | ' ||
		coalesce((SELECT string_agg(diff_type || ':' || coalesce((to_row).v, (from_row).v),
				',' ORDER BY 1)
			FROM dolt_diff('HEAD', 'WORKING', NULL::c.k)), '')};
sub folded
{
	return sql($seen);
}
sub folded_in_full
{
	return sql("SET debug_prolly_force_full_fold = on; $seen");
}
sub agrees
{
	my ($what) = @_;
	my $got = folded();
	is($got, folded_in_full(), "$what: as a fold in full has it");
	return $got;
}
sub commit_holds
{
	my ($what, $working) = @_;
	sql(q{SELECT dolt_commit('-am', 'checked')});
	my $committed = sql(q{
		SELECT coalesce((SELECT string_agg(diff_type || ':' || coalesce((to_row).id, (from_row).id)
				|| ':' || coalesce((to_row).v, '-'), ',' ORDER BY 1)
			FROM dolt_diff('HEAD~1', 'HEAD', NULL::c.t)), '') || ' | ' ||
		coalesce((SELECT string_agg(diff_type || ':' || coalesce((to_row).v, (from_row).v),
				',' ORDER BY 1)
			FROM dolt_diff('HEAD~1', 'HEAD', NULL::c.k)), '')});
	(my $rows = $working) =~ s/^[^|]*\| //;
	is($committed, $rows, "$what: the commit holds the same");
}

my $a = $node->background_psql('postgres', on_error_stop => 0);
my $b = $node->background_psql('postgres', on_error_stop => 0);
$b->query_safe(q{SET prolly.fold_on_commit = on});

# The refs held by a transaction that made a Dolt commit, not committed
# yet: a fold-on writer's COMMIT doesn't wait for them (lock_timeout would
# fail it), and neither deadlocks once the holder writes the same row.
$a->query_safe(q{BEGIN; UPDATE c.t SET v = 'a' WHERE id = 50;
	SELECT dolt_commit('-am', 'a')});
$b->query_safe(q{SET lock_timeout = '500ms'});
$b->query(q{BEGIN; UPDATE c.t SET v = 'b' WHERE id = 1; COMMIT});
is($b->{stderr}, '', 'a fold-on COMMIT behind the refs commits without waiting');
$b->{stderr} = '';
$b->query_safe(q{RESET lock_timeout});
$a->query(q{UPDATE c.t SET v = 'A' WHERE id = 1; COMMIT});
is($a->{stderr}, '', 'and the refs\' holder commits after it, no deadlock');
$a->{stderr} = '';
my $w = agrees('the fold left to the next one');
like($w, qr/modified:1:A/, 'the holder\'s change last');
commit_holds('the fold left to the next one', $w);

# A lazy writer's change of a row, then a fold-on writer's of it and
# another.
$a->query_safe(q{UPDATE c.t SET v = 'lazy' WHERE id = 1});
$b->query_safe(q{UPDATE c.t SET v = 'eager' WHERE id IN (1, 2)});
$w = agrees('a lazy change, then a fold-on one of the same row');
like($w, qr/modified:1:eager,modified:2:eager/, 'the fold-on writer\'s last');
commit_holds('a lazy change, then a fold-on one', $w);

# A lazy writer in flight across fold-on commits, keyed and keyless.
$a->query_safe(q{BEGIN; UPDATE c.t SET v = 'slow' WHERE id = 3;
	DELETE FROM c.k WHERE v = 'k1'});
$b->query_safe(q{UPDATE c.t SET v = 'fast' WHERE id = 4;
	INSERT INTO c.k VALUES ('fast')});
$b->query_safe(q{UPDATE c.t SET v = 'fast again' WHERE id = 5});
agrees('a lazy writer in flight');
$a->query_safe('COMMIT');
$w = agrees('the lazy writer committed after the fold-on ones');
like($w, qr/modified:3:slow/, 'its change kept');
like($w, qr/removed:k1/, 'its keyless delete too');
commit_holds('a lazy writer across fold-on commits', $w);

# A repeatable-read fold-on writer whose snapshot is older than another
# fold-on writer's commit.
my $c = $node->background_psql('postgres', on_error_stop => 0);
$c->query_safe(q{SET prolly.fold_on_commit = on});
$c->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM c.t});
$b->query_safe(q{UPDATE c.t SET v = 'newer' WHERE id = 6});
$c->query_safe(q{UPDATE c.t SET v = 'older snapshot' WHERE id = 7; COMMIT});
$w = agrees('a repeatable-read fold-on writer behind another\'s fold');
like($w, qr/modified:6:newer,modified:7:older snapshot/, 'both changes');
commit_holds('a repeatable-read fold-on writer', $w);

$a->quit;
$b->quit;
$c->quit;
$node->stop;
done_testing();
