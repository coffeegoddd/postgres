# Copyright (c) 2026, PostgreSQL Global Development Group

# Revision-qualified names read another branch's working set as a session
# on it would, beside that branch's own sessions: its writers' committed
# rows under the reader's snapshot, its copy locked against definition
# changes while read, the branch held against deletion, its grants as they
# are now, and a transaction whose snapshot can't move refused where a
# checkout of the branch would be.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('revision_reads');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE ROLE regress_rr_reader;
	CREATE SCHEMA s VERSIONED;
	GRANT USAGE ON SCHEMA s TO regress_rr_reader;
	CREATE TABLE s.t (id int PRIMARY KEY, v int);
	INSERT INTO s.t SELECT g, 1 FROM generate_series(1, 10) g;
	SELECT dolt_commit('-Am', 'one');
	SELECT dolt_branch('b1');
	SELECT dolt_branch('b2');
	SELECT dolt_branch('h1');
});
# b1 and b2 used, each with a session of its own
$node->safe_psql('postgres/b1', q{UPDATE s.t SET v = 2 WHERE id = 1});
$node->safe_psql('postgres/b2', q{UPDATE s.t SET v = 3 WHERE id = 1});

my $sum = q{SELECT (SELECT sum(v) FROM "postgres/b1".s.t),
				   (SELECT sum(v) FROM "postgres/b2".s.t)};
is($node->safe_psql('postgres', $sum), '11|12',
	'each branch read as its working set');

# A writer on b1 with its change uncommitted: not seen; committed: seen by
# the next statement, but not by a snapshot taken before it.
my $w = $node->background_psql('postgres/b1');
my $r = $node->background_psql('postgres', on_error_stop => 0);
$w->query_safe(q{BEGIN; UPDATE s.t SET v = 10 WHERE id = 2});
is($r->query_safe($sum), '11|12', 'an uncommitted write on the branch is not seen');
$r->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ});
is($r->query_safe($sum), '11|12', 'a snapshot taken');
$w->query_safe(q{COMMIT});
is($r->query_safe($sum), '11|12', 'a write committed since the snapshot is not seen');
is($r->query_safe(q{SELECT sum(b1.v) - sum(m.v) FROM s.t m
					JOIN "postgres/b1".s.t b1 USING (id)}),
	'1', 'nor in a join with the session\'s table');
$r->query_safe(q{COMMIT});
is($r->query_safe($sum), '20|12', 'the next transaction sees it');

# While a transaction has read b1's table, b1's definition changes wait
# for it (its copy locked as a session on b1 locks it), and b1 can't be
# deleted (held as a session on it holds it).
$r->query_safe(q{BEGIN; SELECT count(*) FROM "postgres/b1".s.t});
my ($ret, $out, $err) = $node->psql('postgres/b1',
	q{SET lock_timeout = '100ms'; ALTER TABLE s.t ADD COLUMN w int});
like($err, qr/lock timeout/, 'a definition change on the branch waits for the reader');
($ret, $out, $err) = $node->psql('postgres', q{SELECT dolt_branch('-D', 'b2')});
is($ret, 0, 'a branch not read can be deleted');
$r->query_safe(q{SELECT count(*) FROM "postgres/b1".s.t});
$w->quit;
($ret, $out, $err) = $node->psql('postgres', q{SELECT dolt_branch('-D', 'b1')});
isnt($ret, 0, 'a branch a transaction read is held against deletion');
$r->query_safe(q{COMMIT});

# Grants changed on the branch by another session: the next read takes them.
$node->safe_psql('postgres/b1', q{GRANT SELECT ON s.t TO regress_rr_reader});
$r->query_safe(q{SET ROLE regress_rr_reader});
is($r->query_safe(q{SELECT count(*) FROM "postgres/b1".s.t}), '10',
	'the branch\'s grant read');
$node->safe_psql('postgres/b1', q{REVOKE SELECT ON s.t FROM regress_rr_reader});
$r->query(q{SELECT count(*) FROM "postgres/b1".s.t});
like($r->{stderr}, qr/permission denied for table t of revision "b1"/,
	'a grant revoked on the branch is seen at the next read');
$r->{stderr} = '';
$r->query_safe(q{RESET ROLE});

# A branch first used after a transaction's snapshot: its copies' rows are
# ones the snapshot can't see, so the read is refused, as a checkout is.
$r->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1});
$node->safe_psql('postgres/h1', q{SELECT 1});
$r->query(q{SELECT count(*) FROM "postgres/h1".s.t});
like($r->{stderr}, qr/could not serialize access: branch "h1" was first used since/,
	'a branch first used since the snapshot is refused');
$r->{stderr} = '';
$r->query_safe(q{ROLLBACK});
is($r->query_safe(q{SELECT count(*) FROM "postgres/h1".s.t}), '10',
	'and read by the next transaction');

$r->quit;
$node->stop;
done_testing();
