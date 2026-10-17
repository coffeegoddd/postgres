# Copyright (c) 2026, PostgreSQL Global Development Group

# What version control leaves survives a crash: a merge's rows, a hard
# reset's, and a merge's conflicts kept with dolt.allow_commit_conflicts
# (still listed, still blocking another merge, and undone by --abort).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_crash');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }
sub crash { $node->stop('immediate'); $node->start; }

sql(q{
	CREATE SCHEMA bc VERSIONED;
	CREATE TABLE bc.t (id int PRIMARY KEY, v text);
	INSERT INTO bc.t SELECT g, 'base' FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'base');
	SELECT dolt_branch('side');
});

# A three-way merge's rows.
sql(q{UPDATE bc.t SET v = 'side' WHERE id <= 10; SELECT dolt_commit('-am', 'side')},
	'postgres/side');
sql(q{UPDATE bc.t SET v = 'main' WHERE id > 90; SELECT dolt_commit('-am', 'main');
	SELECT dolt_merge('side', '-m', 'merged')});
my $merged = sql(q{SELECT dolt_hashof('HEAD')});
crash();
is(sql(q{SELECT dolt_hashof('HEAD')}), $merged, 'a merge commit survives a crash');
is(sql(q{SELECT string_agg(v || ':' || n, ',' ORDER BY v)
	FROM (SELECT v, count(*) AS n FROM bc.t GROUP BY v) c}),
	'base:80,main:10,side:10', 'and its rows');

# A hard reset's rows.
sql(q{INSERT INTO bc.t SELECT g, 'later' FROM generate_series(101, 120) g;
	SELECT dolt_commit('-am', 'later'); SELECT dolt_reset('--hard', 'HEAD~1')});
crash();
is(sql(q{SELECT dolt_hashof('HEAD')}), $merged, 'a hard reset survives a crash');
is(sql(q{SELECT count(*) FROM bc.t WHERE id > 100}), '0', 'and its rows');

# A merge's conflicts, kept.
sql(q{UPDATE bc.t SET v = 'theirs' WHERE id = 50; SELECT dolt_commit('-am', 'theirs')},
	'postgres/side');
sql(q{UPDATE bc.t SET v = 'ours' WHERE id = 50; SELECT dolt_commit('-am', 'ours');
	SET dolt.allow_commit_conflicts = on; SELECT dolt_merge('side')});
crash();
is(sql(q{SELECT table_name || ':' || num_conflicts FROM dolt_conflicts}), 'bc.t:1',
	'conflicts kept survive a crash');
my ($ret, $out, $err) = $node->psql('postgres', q{SELECT dolt_merge('side')});
like($err, qr/a merge is in progress/, 'and still block another merge');
sql(q{SELECT dolt_merge('--abort')});
is(sql(q{SELECT v FROM bc.t WHERE id = 50}), 'ours', '--abort restores the rows');
is(sql(q{SELECT count(*) FROM dolt_conflicts}), '0', 'and leaves no conflicts');

$node->stop;
done_testing();
