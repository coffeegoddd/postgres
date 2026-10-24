# Copyright (c) 2026, PostgreSQL Global Development Group

# The pages a commit keeps survive a crash at wal_level minimal, where no
# image of them is logged: redo replays the rows written before the commit
# into the pages the snapshot holds, though the working set has moved off
# them since.  A hard reset after the crash gives the head's rows back,
# and a branch first used of those pages before the crash reads them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_pages_crash');
$node->init;
$node->append_conf('postgresql.conf',
	"wal_level = minimal\nmax_wal_senders = 0\nautovacuum = off\nprolly_fold_worker = off\ncheckpoint_timeout = 1h\nprolly.restore_min_changes = 0");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
my $rows = q{SELECT count(*) FILTER (WHERE v = 'v' || id) || '/' || count(*) FROM app.t};

# the rows written after the last checkpoint, so that redo replays them
sql(q{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text)});
sql('CHECKPOINT');
sql(q{INSERT INTO app.t SELECT i, 'v' || i FROM generate_series(1, 5000) i});
sql(q{SELECT dolt_commit('-Am', 'base'); SELECT dolt_branch('b')});

# b's first use made of the pages main keeps as of its head
sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}
my $kept = counted('first_use_tables_kept');
is($node->safe_psql('postgres/b', $rows), '5000/5000', 'b as committed');
cmp_ok(counted('first_use_tables_kept'), '>', $kept, 'made of the pages main keeps');

# the working set moves off the kept pages: each page it changes remapped
sql(q{UPDATE app.t SET v = 'changed' WHERE id % 3 = 0});
is(sql($rows), '3334/5000', 'main changed');

$node->stop('immediate');
$node->start;
is(sql($rows), '3334/5000', 'after a crash, the working set as it was');
is($node->safe_psql('postgres/b', $rows), '5000/5000', 'and b as committed');
my $restored = counted('relations_restored');
sql(q{SELECT dolt_reset('--hard')});
cmp_ok(counted('relations_restored'), '>', $restored, 'the reset gave the kept pages back');
is(sql($rows), '5000/5000', 'and they hold the head\'s rows');

$node->stop;
done_testing();
