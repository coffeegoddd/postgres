# Copyright (c) 2026, PostgreSQL Global Development Group

# dolt.transaction_commit's commits survive a crash: the head and the rows
# agree after recovery, a transaction that didn't commit left no commit,
# and a standby replays the same head.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('transaction_commits');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('transaction_commits_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub crash { $node->stop('immediate'); $node->start; }

sql(q{
	CREATE SCHEMA tc VERSIONED;
	CREATE TABLE tc.t (id int PRIMARY KEY, v text);
	SELECT dolt_commit('-Am', 'base');
	ALTER DATABASE postgres SET dolt.transaction_commit = on;
});

sql(q{INSERT INTO tc.t SELECT g, 'one' FROM generate_series(1, 50) g});
sql(q{BEGIN; UPDATE tc.t SET v = 'two' WHERE id <= 10; COMMIT});
my $head = sql(q{SELECT dolt_hashof('HEAD')});
crash();
is(sql(q{SELECT dolt_hashof('HEAD')}), $head, 'a transaction commit survives a crash');
is(sql(q{SELECT count(*) FROM dolt_log('main')}), '3', 'and so does the one before it');
is(sql(q{SELECT string_agg(v || ':' || n, ',' ORDER BY v)
	FROM (SELECT v, count(*) AS n FROM tc.t GROUP BY v) c}),
	'one:40,two:10', 'and its rows');
is(sql(q{SELECT dirty FROM dolt_branches() WHERE name = 'main'}), 'f',
	'the rows are the head\'s');

# A transaction in flight at the crash: no rows, and no commit.
my $bg = $node->background_psql('postgres');
$bg->query_safe(q{BEGIN; INSERT INTO tc.t VALUES (100, 'in flight')});
$node->stop('immediate');
$bg->quit;
$node->start;
is(sql(q{SELECT dolt_hashof('HEAD')}), $head, 'one in flight makes no commit');
is(sql(q{SELECT count(*) FROM tc.t WHERE id = 100}), '0', 'and no rows');

# A standby replays the head and its rows.
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', q{SELECT dolt_hashof('HEAD')}), $head,
	'a standby has the same head');
is($standby->safe_psql('postgres', q{SELECT count(*) FROM tc.t}), '50',
	'and its rows');

$standby->stop;
$node->stop;
done_testing();
