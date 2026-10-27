# Copyright (c) 2026, PostgreSQL Global Development Group

# Under dolt.transaction_commit, a root due at a transaction's commit is
# kept only if no other writer of the branch holds its tables.  A keep
# that can't be had is not tried again within prolly.root_interval: each
# try reads the branch's kept commits and asks what is left of them, and
# every transaction's commit would try it while another writer works.
# The interval is the branch's, shared by every backend: once one keeps
# a root, the others' commits within the interval read nothing of the
# kept commits either.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

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
sql('CREATE EXTENSION injection_points');

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

# the interval passed, one backend's commit keeps a root; the commits of
# three others within the interval (a long one, from then), each its
# first, read no kept commits
sleep(3);
$kept = counted('pages_kept');
my $first = $node->background_psql('postgres');
$first->query_safe(q{SET dolt.transaction_commit = on; SET prolly.root_interval = '2s'});
$first->query_safe('UPDATE rk.t SET v = 100 WHERE id = 2');
$first->query_safe('SELECT pg_stat_force_next_flush()');
cmp_ok(counted('pages_kept'), '>', $kept, 'one backend keeps a root');
my $read = 0;
for my $i (1 .. 3)
{
	my $o = $node->background_psql('postgres', on_error_stop => 0);
	$o->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-root-kept-read', 'notice')});
	$o->query_safe(q{SET dolt.transaction_commit = on; SET prolly.root_interval = '1h'});
	$o->query("UPDATE rk.t SET v = 100 + $i WHERE id = 3");
	$read++ if $o->{stderr} =~ /prolly-root-kept-read/;
	$o->quit;
}
is($read, 0, 'the others, within the interval, read no kept commits');
$first->quit;
$node->stop;
done_testing();
