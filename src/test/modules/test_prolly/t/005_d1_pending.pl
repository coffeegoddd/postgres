# Copyright (c) 2026, PostgreSQL Global Development Group

# Roots waiting to be published or abandoned don't outlive their
# transactions.  A CREATE DATABASE that aborts drops the heads it copied,
# and one that commits publishes them.  Entries nobody settles (here, the
# ones a standby replayed but nobody read) are settled by a checkpoint
# once the server is a primary, and left alone by restartpoints.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# No timed checkpoints: they would settle entries the tests count.
my $conf = "default_table_access_method = prolly_d1\n"
  . "checkpoint_timeout = '1h'\n";

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $conf);
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
$primary->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', $conf);
$standby->start;

my $usage = q{SELECT heads || ',' || pending FROM prolly_commit_usage('serial')};

$primary->safe_psql('postgres', 'CREATE DATABASE src');
$primary->safe_psql('src', q{
	CREATE TABLE t (id int PRIMARY KEY);
	INSERT INTO t VALUES (1), (2);
});
is($primary->safe_psql('postgres', $usage), '1,0', 'one head, nothing pending');

# A CREATE DATABASE that fails as it commits, after copying the heads.
my ($ret, $stdout, $stderr) = $primary->psql('postgres', q{
	SELECT prolly_fail_next_commit();
	CREATE DATABASE failed TEMPLATE src;
});
like($stderr, qr/commit failed by prolly_fail_next_commit/,
	'CREATE DATABASE fails at commit');
is($primary->safe_psql('postgres', $usage), '1,0',
	'the failed copy left nothing pending');

# One that commits publishes its heads at once.
$primary->safe_psql('postgres', 'CREATE DATABASE copied TEMPLATE src');
is($primary->safe_psql('postgres', $usage), '2,0',
	'a committed copy publishes its head');
is($primary->safe_psql('copied', 'SELECT count(*) FROM t'), '2',
	'and has the template\'s rows');

# The standby replays commits to a database nobody reads there; the last
# commit to each branch stays pending.
$primary->safe_psql('src', 'INSERT INTO t VALUES (3)');
$primary->safe_psql('copied', 'INSERT INTO t VALUES (3), (4)');
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->wait_for_replay_catchup($standby);
my ($heads, $pending) =
  split /,/, $standby->safe_psql('postgres', $usage);
cmp_ok($pending, '>', 0, 'the standby holds replayed entries nobody settled');

$standby->safe_psql('postgres', 'CHECKPOINT');
is($standby->safe_psql('postgres', $usage), "$heads,$pending",
	'a restartpoint leaves them pending');

# Once promoted, a checkpoint settles them.
$standby->promote;
$standby->safe_psql('postgres', 'CHECKPOINT');
($heads, $pending) = split /,/, $standby->safe_psql('postgres', $usage);
is($pending, '0', 'a checkpoint on the promoted standby settles every entry');
is($standby->safe_psql('src', 'SELECT count(*) FROM t'), '3',
	'the settled heads have every commit (src)');
is($standby->safe_psql('copied', 'SELECT count(*) FROM t'), '4',
	'the settled heads have every commit (copied)');

done_testing();
