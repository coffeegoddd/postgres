# Copyright (c) 2026, PostgreSQL Global Development Group

# Branches of prolly data: each branch has its own working set, which a
# session's checkout selects; commits record a working set, and a branch
# can start from any commit.  Refs and working sets survive a crash and
# replay on a standby; prolly_gc() keeps what only an old commit reaches;
# writers of one row on different branches don't wait for each other; a
# prepared transaction commits to the branch it wrote.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branches');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
default_table_access_method = prolly_d1
max_prepared_transactions = 10
});
$node->start;
$node->safe_psql('postgres',
	q{SELECT pg_create_physical_replication_slot('standby')});
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('branches_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'standby'\n");
$standby->start;

sub rows
{
	my ($n, $branch) = @_;
	return $n->safe_psql('postgres', qq{
		SELECT prolly_checkout('$branch');
		SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM t;});
}

$node->safe_psql('postgres', q{
	CREATE TABLE t (id int PRIMARY KEY, v text);
	INSERT INTO t VALUES (1, 'base');
});
my $c1 = $node->safe_psql('postgres', q{SELECT prolly_commit('base')});
$node->safe_psql('postgres', q{
	SELECT prolly_branch_create('feature', NULL);
	SELECT prolly_checkout('feature');
	UPDATE t SET v = 'feature' WHERE id = 1;
	INSERT INTO t VALUES (2, 'feature');
});
is(rows($node, 'feature'), "\n1:feature,2:feature", 'a branch has its own rows');
is(rows($node, 'main'), "\n1:base", 'the other branch keeps its own');

# Writers of one row on two branches don't wait for each other.
my $s1 = $node->background_psql('postgres');
$s1->query_safe(q{SELECT prolly_checkout('feature');
	BEGIN; UPDATE t SET v = 'locked' WHERE id = 1;});
is($node->safe_psql('postgres', q{SET lock_timeout = '2s';
	UPDATE t SET v = 'main' WHERE id = 1 RETURNING v;}), 'main',
	'a row locked on one branch can be written on another');
$s1->query_safe('ROLLBACK');

# A prepared transaction commits to the branch it wrote.
$s1->query_safe(q{BEGIN; INSERT INTO t VALUES (3, 'prepared');
	PREPARE TRANSACTION 'p';});
$node->safe_psql('postgres', q{COMMIT PREPARED 'p'});
is(rows($node, 'feature'), "\n1:feature,2:feature,3:prepared",
	'a prepared transaction commits to its branch');
is(rows($node, 'main'), "\n1:main", 'and not to the session\'s');
$s1->quit;

# A branch from an old commit sees what the commit recorded, after a
# collection that only that commit's refs kept.
$node->safe_psql('postgres', q{UPDATE t SET v = 'later'; SELECT prolly_commit('later');});
$node->safe_psql('postgres', 'SELECT * FROM prolly_gc()');
$node->safe_psql('postgres', qq{SELECT prolly_branch_create('old', '$c1')});
is(rows($node, 'old'), "\n1:base", 'a branch starts from an old commit after prolly_gc');

# Through a crash, and on a standby.
my $all = sub {
	my ($n) = @_;
	return join ' ', map { rows($n, $_) } qw(main feature old);
};
my $before = $all->($node);
$node->stop('immediate');
$node->start;
is($all->($node), $before, 'branches and their rows survive a crash');
$node->wait_for_replay_catchup($standby);
is($all->($standby), $before, 'a standby has the same branches');

my ($ret, $out, $err) = $node->psql('postgres', q{SELECT prolly_checkout('nope')});
like($err, qr/prolly branch "nope" does not exist/, 'an unknown branch is an error');

$standby->stop;
$node->stop;
done_testing();
