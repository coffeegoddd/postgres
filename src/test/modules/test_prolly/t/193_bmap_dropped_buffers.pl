# Copyright (c) 2026, PostgreSQL Global Development Group

# Buffers of blocks given other contents.  A tree's node that a drop frees
# keeps its page in a buffer, dirty, under the page store's tag.  A block
# copied on write that takes the node's block again has that buffer dropped
# first, on the primary and in a standby's replay, so the stale node page is
# never written over the data page now there.  Dropping one block's buffer
# drops that block's page alone, even when the buffer it found is given
# another block of the same fork meanwhile, and it waits out a reader's pin
# as a cleanup lock does, rather than spinning on it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('bmap_dropped_buffers');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
shared_buffers = 16MB
checkpoint_timeout = 1h
wal_keep_size = '1GB'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	 CREATE EXTENSION pg_buffercache; CREATE EXTENSION pg_prewarm');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('bmap_dropped_buffers_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub digest
{
	my ($n, $rel) = @_;
	return $n->safe_psql('postgres',
		qq{SELECT count(*) || ':' || coalesce(sum(a), 0) || ':' ||
			coalesce(md5(string_agg(b, ',' ORDER BY a)), '') FROM $rel});
}

sub catch_up
{
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
}

my $db = q{(SELECT oid FROM pg_database WHERE datname = current_database())};

# a table of two blocks, shared, so that a change copies its blocks
sql(q{CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, repeat('t', 100) FROM generate_series(1, 100) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
sql(qq{SELECT test_bmap_share($rel)});
sql('CHECKPOINT');

# another table's leaf, dirty in its buffer, freed by the table's drop
sql(q{CREATE TABLE x (a int, b text);
	INSERT INTO x SELECT g, repeat('x', 100) FROM generate_series(1, 100) g});
my $leaf = sql(q{SELECT block FROM test_bmap_tree(pg_relation_filenode('x'))});
my $leaf_dirty = qq{SELECT isdirty FROM pg_buffercache
	WHERE reldatabase = $db AND relforknumber = 0 AND relfilenode = 9421 AND relblocknumber = $leaf};
is(sql($leaf_dirty), 't', 'a leaf\'s page is in its buffer, dirty');
catch_up();
is($standby->safe_psql('postgres', $leaf_dirty), 't',
	'and in the standby\'s, replayed');
sql('DROP TABLE x');

# copies on write take the freed blocks, the leaf's among them
sql(q{UPDATE t SET b = 'u' WHERE a IN (1, 100)});
is(sql(qq{SELECT count(*) FROM test_bmap_map($rel) WHERE phys = $leaf}),
	'1', 'a copy on write takes the freed leaf\'s block');
my $rows = digest($node, 't');
catch_up();

# the data pages written first, then whatever else is dirty
sql(q{SELECT pg_buffercache_evict_relation('t')});
$standby->safe_psql('postgres', q{SELECT pg_buffercache_evict_relation('t')});
sql('CHECKPOINT');
catch_up();
$standby->safe_psql('postgres', 'CHECKPOINT');
$node->restart;
$standby->restart;
is(digest($node, 't'), $rows, 'the copied page is read back after a restart');
catch_up();
is(digest($standby, 't'), $rows, 'and on the standby');

# one block's buffer dropped, which is given another block of the same
# fork between its lookup and the drop, and that block's page changed:
# the change stays.  (Few buffers, so that one is soon given another block,
# and room on each page, so that the change stays on its page.)
$node->append_conf('postgresql.conf', 'shared_buffers = 1MB');
$node->restart;
sql(q{CREATE TABLE big (a int, b text) WITH (fillfactor = 50);
	INSERT INTO big SELECT g, repeat('b', 500) FROM generate_series(1, 2000) g});
my $big = sql(q{SELECT pg_relation_filenode('big')});
my $drop = $node->background_psql('postgres');
$drop->query_safe(q{SELECT injection_points_set_local()});
$drop->query_safe(
	q{SELECT injection_points_attach('drop-buffer-block-found', 'wait')});
# (read often, so that it isn't evicted before the drop finds it)
$drop->query_until(qr/dropping/, qq{\\echo dropping
SELECT pg_prewarm('big', 'buffer', 'main', 0, 0) FROM generate_series(1, 5);
SELECT test_bmap_drop_buffer($big, 0);
});
$node->wait_for_event('client backend', 'drop-buffer-block-found');
my $buf = sql(qq{SELECT bufferid FROM pg_buffercache
	WHERE reldatabase = $db AND relforknumber = 0 AND relfilenode = $big
	AND relblocknumber = 0});
sql(qq{SELECT pg_buffercache_evict($buf)});
sql(qq{DO \$\$
DECLARE
	blk int8;
BEGIN
	FOR pass IN 1 .. 5 LOOP
		FOR i IN 1 .. pg_relation_size('big') / 8192 - 1 LOOP
			PERFORM pg_prewarm('big', 'buffer', 'main', i, i);
			SELECT relblocknumber INTO blk FROM pg_buffercache
			WHERE bufferid = $buf AND reldatabase = $db
			AND relforknumber = 0 AND relfilenode = $big;
			IF blk IS NOT NULL THEN
				UPDATE big SET b = 'changed' WHERE ctid = format('(%s,1)', blk)::tid;
				RETURN;
			END IF;
		END LOOP;
	END LOOP;
	RAISE 'the buffer was given no other block of the table';
END \$\$});
is( sql(qq{SELECT count(*) FROM pg_buffercache
	WHERE bufferid = $buf AND reldatabase = $db AND relforknumber = 0
	AND relfilenode = $big AND relblocknumber > 0 AND isdirty}),
	'1',
	'the buffer holds another block of the table, changed');
$node->safe_psql('postgres',
	q{SELECT injection_points_detach('drop-buffer-block-found')});
$node->safe_psql('postgres',
	q{SELECT injection_points_wakeup('drop-buffer-block-found')});
$drop->quit;
is(sql(q{SELECT count(*) FROM big WHERE b = 'changed'}),
	'1', 'that other block\'s change is not dropped with block 0\'s page');

# a reader's pin, held by a cursor stopped on the page, is waited for as
# a cleanup lock waits, and the page is dropped once it is let go of
sql(q{SELECT pg_prewarm('big', 'buffer', 'main', 0, 0)});
my $cursor = $node->background_psql('postgres');
$cursor->query_safe(q{SET synchronize_seqscans = off});
$cursor->query_safe(q{BEGIN});
$cursor->query_safe(q{DECLARE c CURSOR FOR SELECT a FROM big});
$cursor->query_safe(q{FETCH 1 FROM c});
my $waiter = $node->background_psql('postgres');
$waiter->query_until(qr/dropping/, qq{\\echo dropping
SELECT test_bmap_drop_buffer($big, 0);
});
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'BufferCleanup'}),
	'the drop waits for the reader\'s pin');
$cursor->query_safe(q{COMMIT});
ok( $node->poll_query_until(
		'postgres', qq{SELECT count(*) = 0 FROM pg_buffercache
		WHERE reldatabase = $db AND relforknumber = 0 AND relfilenode = $big AND relblocknumber = 0}),
	'and drops the page once the pin is let go of');
$waiter->quit;
$cursor->quit;

$standby->stop;
$node->stop;
done_testing();
