# Copyright (c) 2026, PostgreSQL Global Development Group

# A page sweep counts every map of the store, a dropped relation's too until
# it is unlinked.  A table whose drop has committed and dropped its buffers,
# its files not yet unlinked, is read by a sweep run meanwhile: its map's
# pages come back into shared buffers.  Once the files are unlinked none of
# them is left there, where a relation given its file number after the next
# checkpoint would find them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('sweep_dropped_map');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE EXTENSION pg_buffercache;
	CREATE SCHEMA kv VERSIONED;
	CREATE TABLE kv.t1 (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.t1 SELECT g, repeat('y', 100) FROM generate_series(1, 3000) g;
	CHECKPOINT;
});

my $fn = sql(q{SELECT pg_relation_filenode('kv.t1')});
my $cached = qq{SELECT count(*) FROM pg_buffercache WHERE relfilenode = $fn};
my $cached_map = qq{SELECT count(*) FROM pg_buffercache
	WHERE relfilenode = $fn AND relforknumber > 0};

# t1 dropped: committed, its buffers dropped, its files not yet unlinked
my $drop = $node->background_psql('postgres');
$drop->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-drops-done', 'wait')});
$drop->query_until(qr/started/, qq{
	\\echo started
	DROP TABLE kv.t1;
	\\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-drops-done'}) or die 'the drop did not stop';
is(sql($cached), '0', 'the drop dropped its buffers');

# a sweep meanwhile counts t1's map, reading it back into shared buffers
is(sql(q{SELECT count(*) FROM prolly_page_sweep()}), '1', 'a sweep runs in the window');
cmp_ok(sql($cached_map), '>', 0, 'the sweep read the dropped map');

# the drop ends: none of t1's pages stays cached
sql(q{SELECT injection_points_wakeup('bmap-drops-done')});
$drop->query_until(qr/done/, '');
is(sql($cached), '0', 'none of the dropped map\'s pages stays cached after the unlink');

# a sweep after finds nothing of t1 either
sql(q{SELECT count(*) FROM prolly_page_sweep()});
is(sql($cached), '0', 'nor after a later sweep');

$drop->quit;
$node->stop;
done_testing();
