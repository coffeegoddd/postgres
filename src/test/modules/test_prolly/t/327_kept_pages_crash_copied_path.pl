# Copyright (c) 2026, PostgreSQL Global Development Group

# A crash at wal_level minimal (no images of kept pages logged) after a
# change copied the map path of pages a commit kept.  The change copies
# the leaf the kept pages share; the root page naming the new leaf is
# written out, the leaf itself isn't.  Redo then finds the leaf it names
# missing and skips the records of every block under it from before the
# copy (tree_find()), relying on the snapshot having written its pages
# out.  Pages dirty at the commit that the change didn't copy are written
# out only by the snapshot (the pages changed since the last one), so
# without that their rows are lost, in the working set and in the kept
# pages alike.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_pages_crash_copied_path');
$node->init;
$node->append_conf('postgresql.conf', qq{
wal_level = minimal
max_wal_senders = 0
wal_consistency_checking = all
shared_buffers = 64MB
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}

# (pages of one leaf, written out)
sql(q{
	CREATE EXTENSION pg_buffercache;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text) WITH (fillfactor = 10);
	INSERT INTO app.t SELECT g, 0, repeat('x', 100) FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'base');
});
sql('CHECKPOINT');
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.t};

# rows of the first pages changed after the checkpoint and committed: kept
sql(q{UPDATE app.t SET v = 1 WHERE id <= 40});
sql(q{SELECT dolt_commit('-am', 'first pages')});
my $at = sql($digest);

# a page far from them changed: its leaf's path copied
sql(q{UPDATE app.t SET v = -1 WHERE id = 2000});
my $now = sql($digest);

# the root page naming the copy written out, the copy not
my $evicted = sql(q{
	SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted) FROM pg_buffercache
	WHERE relfilenode = pg_relation_filenode('app.t')
		AND relforknumber = 4 AND relblocknumber = 1});
is($evicted, 't', 'the root page written out and evicted');

$node->stop('immediate');
$node->start;
is(sql($digest), $now, 'after a crash, the working set as it was');
my $restored = counted('relations_restored');
sql(q{SELECT dolt_reset('--hard')});
cmp_ok(counted('relations_restored'), '>', $restored,
	'the reset gave the kept pages back');
is(sql($digest), $at, "and they hold the commit's rows");

$node->stop;
done_testing();
