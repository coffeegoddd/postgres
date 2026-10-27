# Copyright (c) 2026, PostgreSQL Global Development Group

# The background sever holds no ref while it plans and archives: commits
# go on meanwhile, none waiting on the autovacuum worker (each would wait
# only lock_timeout, then fail), and the worker isn't cancelled by them,
# so the fragment is made.  The archive is slow, a tenth of a second and
# more a chunk, so commits land while it sends.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('auto_commits');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
shared_preload_libraries = 'test_prolly'
deadlock_timeout = 100ms
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE ac');
sub sql { return $node->safe_psql('ac', $_[0]); }

sql(q{CREATE TABLE visits (n int) USING heap WITH
	(autovacuum_vacuum_insert_threshold = 0,
	 autovacuum_vacuum_insert_scale_factor = 0);
	CREATE SCHEMA ac VERSIONED;
	CREATE TABLE ac.t (id int PRIMARY KEY, v text);
	INSERT INTO ac.t SELECT g, 'v' || g FROM generate_series(1, 300) g;});
for my $ago (reverse 4 .. 10)
{
	sql(qq{UPDATE ac.t SET v = 'd$ago' WHERE id % 13 = $ago;
		SELECT dolt_commit('-Am', 'd$ago', '--date',
			(now() - interval '$ago days')::text)});
}
my $slow = 'slow://' . PostgreSQL::Test::Utils::tempdir() . '/slow';
sql(qq{ALTER DATABASE ac SET prolly.history_window = '3 days';
	ALTER DATABASE ac SET prolly.history_batch = '1 second';
	ALTER DATABASE ac SET prolly.history_collect_interval = '1 second';
	ALTER DATABASE ac SET prolly.history_archive = '$slow'});

# Commits while the worker severs, each refused past a short lock wait.
my ($committed, $refused) = (0, 0);
my $until = time() + 120;
for (my $i = 1; time() < $until; $i++)
{
	my ($ret, $out, $err) = $node->psql('ac', qq{SET lock_timeout = '300ms';
		UPDATE ac.t SET v = 'now $i' WHERE id = 1;
		SELECT dolt_commit('-am', 'now $i');
		INSERT INTO visits VALUES ($i);});
	if ($ret == 0) { $committed++; } else { $refused++; diag($err); }
	last if sql(q{SELECT count(*) FROM prolly_history_horizon()}) ne '0';
	select(undef, undef, undef, 0.2);
}
cmp_ok($committed, '>', 5, 'commits went on while the worker severed');
is($refused, 0, 'no commit waited on the worker');
isnt(sql(q{SELECT count(*) FROM prolly_history_horizon()}), '0',
	'the worker made its fragment');
ok(!$node->log_contains(qr/canceling autovacuum task/),
	'the worker wasn\'t cancelled by a commit waiting on it');

done_testing();
