# Copyright (c) 2026, PostgreSQL Global Development Group

# A page sweep lowering shared_below reads a relation's map only once it
# holds the relation against its drop.  A table dropped while the sweep
# runs, its buffers dropped and its files not yet unlinked, is passed
# over: none of its map's pages is left in shared buffers, where a
# relation given its file number later would find them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('shared_below_drop');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE EXTENSION pg_buffercache;
	CREATE SCHEMA kv VERSIONED;
	CREATE TABLE kv.t9 (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.t9 SELECT g, repeat('y', 100) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'c1');
	SELECT dolt_branch('c');
});

# c first used of main's kept pages: main's t9 has shared_below set
is($node->safe_psql('postgres/c', 'SELECT count(*) FROM kv.t9'), '3000', 'c is first used');
my $fn = sql(q{SELECT pg_relation_filenode('kv.t9')});
my $cached = qq{SELECT count(*) FROM pg_buffercache WHERE relfilenode = $fn};

# the sweep stops once it has counted, before it lowers anything
my $sweep = $node->background_psql('postgres');
$sweep->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-sweep-counted', 'wait')});
$sweep->query_until(qr/started/, qq{
	\\echo started
	SELECT count(*) FROM prolly_page_sweep();
	\\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-sweep-counted'}) or die 'the sweep did not stop';

# t9 dropped: committed, its buffers dropped, its files not yet unlinked
my $drop = $node->background_psql('postgres');
$drop->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-drops-done', 'wait')});
$drop->query_until(qr/started/, qq{
	\\echo started
	DROP TABLE kv.t9;
	\\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-drops-done'}) or die 'the drop did not stop';
is(sql($cached), '0', 'the drop dropped its buffers');

# the sweep goes on: t9 passed over, nothing of it read
sql(q{SELECT injection_points_wakeup('bmap-sweep-counted')});
$sweep->query_until(qr/done/, '');
is(sql($cached), '0', 'the sweep read none of the dropped table\'s pages');

# the drop ends: still nothing of it cached
sql(q{SELECT injection_points_wakeup('bmap-drops-done')});
$drop->query_until(qr/done/, '');
is(sql($cached), '0', 'nor is any after its files are unlinked');

# and a sweep after lowers what is left to lower, as ever
is(sql(q{SELECT count(*) > 0 FROM prolly_page_sweep()}), 't', 'a later sweep runs');

$sweep->quit;
$drop->quit;
$node->stop;
done_testing();
