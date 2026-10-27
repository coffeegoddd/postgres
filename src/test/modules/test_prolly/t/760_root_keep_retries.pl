# Copyright (c) 2026, PostgreSQL Global Development Group

# Under dolt.transaction_commit, a root due at a transaction's commit is
# kept only if no other writer of the branch holds its tables.  A keep
# that can't be had is not tried again within prolly.root_interval: each
# try reads the branch's kept commits and asks what is left of them, and
# every transaction's commit would try it while another writer works.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('root_keep_retries');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT pg_stat_force_next_flush();
		SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = '$name'), 0)});
}

sql(q{CREATE SCHEMA rk VERSIONED;
	CREATE TABLE rk.t (id int PRIMARY KEY, v int);
	CREATE TABLE rk.w (id int PRIMARY KEY, v int);
	INSERT INTO rk.t SELECT g, 0 FROM generate_series(1, 100) g;
	INSERT INTO rk.w VALUES (1, 0);
	SELECT dolt_commit('-Am', 'c0')});

# another writer of the branch, at work: it holds rk.w
my $writer = $node->background_psql('postgres');
$writer->query_safe(q{BEGIN; UPDATE rk.w SET v = 1});

# a root is due (the interval passed since c0 was kept), but every keep
# finds rk.w held
sleep(3);
my $checks = counted('kept_status_checks');
my $kept = counted('pages_kept');
my $s = $node->background_psql('postgres');
$s->query_safe(q{SET dolt.transaction_commit = on; SET prolly.root_interval = '2s'});
$s->query_safe("UPDATE rk.t SET v = $_ WHERE id = 1") for 1 .. 50;
$s->query_safe('SELECT pg_stat_force_next_flush()');
is(counted('pages_kept'), $kept, 'no keep had');
cmp_ok(counted('kept_status_checks') - $checks, '<', 50,
	'one keep tried in the interval, not one for each commit');

$writer->query_safe('COMMIT');
$writer->quit;
$s->quit;
$node->stop;
done_testing();
