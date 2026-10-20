# Copyright (c) 2026, PostgreSQL Global Development Group

# A repeatable-read transaction whose snapshot predates another session's
# first use of a branch can't see that branch's copies (rows written
# after its snapshot), so it would read the branch as the one it was
# copied from: checking it out is refused, to be retried.  Retried, the
# branch is read as it is.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('rr_branch_first_use');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t VALUES (1, 'main');
	SELECT dolt_commit('-Am', 'one');
	SELECT dolt_branch('x')});

my $rr = $node->background_psql('postgres', on_error_stop => 0);
$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is($rr->query_safe('SELECT v FROM s.t'), 'main', 'the snapshot is taken on main');

# another session uses x first, and changes it
sql(q{SELECT dolt_checkout('x'); UPDATE s.t SET v = 'x'; SELECT dolt_commit('-am', 'on x')});

my ($out, $err) = ('', '');
$out = $rr->query(q{SELECT dolt_checkout('x')});
like($rr->{stderr}, qr/branch "x" was first used since this transaction's snapshot/,
	'checking it out by the old snapshot is refused');
$rr->{stderr} = '';
$rr->query_safe('ROLLBACK');

# retried: the branch as it is
$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$rr->query_safe(q{SELECT dolt_checkout('x')});
is($rr->query_safe('SELECT v FROM s.t'), 'x', 'retried, it reads the branch\'s rows');
is($rr->query_safe('SELECT count(*) FROM dolt_status'), '0', 'and its status');
$rr->query_safe('COMMIT');
$rr->quit;

$node->stop;
done_testing();
