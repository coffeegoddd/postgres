# Copyright (c) 2026, PostgreSQL Global Development Group

# SERIALIZABLE on prolly tables: reads are recorded per table with
# predicate.c, and a reader's conflicts with writers it doesn't see (still
# running, or committed after its snapshot) are found, so write skew fails
# to serialize whichever order the reads and writes come in, while
# transactions on different tables commit.  Temporary and unlogged tables
# get heap when prolly is the default, an explicit USING prolly on one is
# refused, and a concurrent refresh of a prolly materialized view (which
# diffs through a temporary table) works.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('serializable');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method = prolly_d1\n");
$node->start;

$node->safe_psql('postgres', q{
	CREATE TABLE oncall (doctor text PRIMARY KEY, on_call bool);
	CREATE TABLE other (id int PRIMARY KEY, v int);
	CREATE TABLE x (id int PRIMARY KEY, v int);
	CREATE TABLE y (id int PRIMARY KEY, v int);
	INSERT INTO other VALUES (1, 0);
	INSERT INTO x VALUES (1, 0);
	INSERT INTO y VALUES (1, 0);
});

my $s1 = $node->background_psql('postgres', on_error_stop => 0);
my $s2 = $node->background_psql('postgres', on_error_stop => 0);

sub reset_oncall
{
	$node->safe_psql('postgres', q{
		DELETE FROM oncall;
		INSERT INTO oncall VALUES ('alice', true), ('bob', true);
	});
}

# Run the transactions' last statements and commits; count failures.
sub failures
{
	my @steps = @_;
	my $failed = 0;

	for my $step (@steps)
	{
		my ($s, $sql) = @$step;
		$s->query($sql);
		$failed++ if $s->{stderr} =~ /could not serialize access/;
		$s->{stderr} = '';
	}
	$s1->query('ROLLBACK');
	$s2->query('ROLLBACK');
	$s1->{stderr} = $s2->{stderr} = '';
	return $failed;
}

my $leave_ok = q{SELECT count(*) FROM oncall WHERE on_call};

# Both read, then both write: the second writer's write finds the first's
# read.
reset_oncall();
$s1->query_safe("BEGIN ISOLATION LEVEL SERIALIZABLE; $leave_ok;");
$s2->query_safe("BEGIN ISOLATION LEVEL SERIALIZABLE; $leave_ok;");
is(failures(
	[$s1, q{UPDATE oncall SET on_call = false WHERE doctor = 'alice'}],
	[$s2, q{UPDATE oncall SET on_call = false WHERE doctor = 'bob'}],
	[$s1, 'COMMIT'], [$s2, 'COMMIT']), 1,
	'write skew after both read fails to serialize');

# A reader that comes after a running writer's write finds it.
reset_oncall();
$s1->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT 1;');
$s2->query_safe(q{BEGIN ISOLATION LEVEL SERIALIZABLE;
	UPDATE oncall SET on_call = false WHERE doctor = 'bob';});
is(failures(
	[$s1, $leave_ok],
	[$s1, q{UPDATE oncall SET on_call = false WHERE doctor = 'alice'}],
	[$s2, $leave_ok],
	[$s1, 'COMMIT'], [$s2, 'COMMIT']), 1,
	'a read after a running transaction\'s write finds the conflict');

# A reader whose snapshot is older than a writer's commit finds it.
reset_oncall();
$s1->query_safe(q{BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT * FROM other;});
$s2->query_safe(qq{BEGIN ISOLATION LEVEL SERIALIZABLE; $leave_ok;
	UPDATE oncall SET on_call = false WHERE doctor = 'bob'; COMMIT;});
is(failures(
	[$s1, $leave_ok],
	[$s1, q{UPDATE oncall SET on_call = false WHERE doctor = 'alice'}],
	[$s1, 'COMMIT']), 1,
	'a read missing a commit after its snapshot finds the conflict');
is($node->safe_psql('postgres', $leave_ok), '1',
	'someone stays on call');

# Transactions on different tables don't conflict.
$s1->query_safe(q{BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT * FROM x;
	UPDATE x SET v = 1;});
$s2->query_safe(q{BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT * FROM y;
	UPDATE y SET v = 1;});
is(failures([$s1, 'COMMIT'], [$s2, 'COMMIT']), 0,
	'transactions on different tables both commit');

# Temporary and unlogged tables.
is($node->safe_psql('postgres', q{
	CREATE TEMP TABLE tt (id int); CREATE UNLOGGED TABLE ut (id int);
	SELECT string_agg(amname, ',' ORDER BY relname) FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam WHERE relname IN ('tt', 'ut');}),
	'heap,heap', 'temporary and unlogged tables get heap');
my ($ret, $out, $err) = $node->psql('postgres',
	'CREATE TEMP TABLE tp (id int) USING prolly_d1');
like($err, qr/prolly storage supports only permanent tables/,
	'an explicit USING prolly on a temporary table is refused');
$node->safe_psql('postgres', q{
	CREATE MATERIALIZED VIEW mv AS SELECT id, v FROM x;
	CREATE UNIQUE INDEX ON mv (id);
	INSERT INTO x VALUES (2, 2);
	REFRESH MATERIALIZED VIEW CONCURRENTLY mv;
});
is($node->safe_psql('postgres', 'SELECT count(*) FROM mv'), '2',
	'a concurrent refresh of a prolly materialized view works');

$s1->quit;
$s2->quit;
$node->stop;
done_testing();
