# Copyright (c) 2026, PostgreSQL Global Development Group

# The page sweep frees what a crash leaked: blocks a prune let go of but
# didn't free, and those of a relation dropped but not yet freed.  It
# holds branch creation off while it counts, and a standby follows what it
# frees and the store it shortens.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('sweep');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE sw');
$node->safe_psql('sw', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.t (a int8 PRIMARY KEY, b int8);
	INSERT INTO cow.t SELECT g, g FROM generate_series(1, 3000) g;
});
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('sweep_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('sw', $_[0]); }
my $sweep = q{SELECT freed || ' ' || recounted FROM prolly_page_sweep()};
my $sum = q{SELECT count(*) || ' ' || sum(b) FROM cow.t};
my $want = sql($sum);
my $rel = sql(q{SELECT pg_relation_filenode('cow.t')});

# A prune stopped by a crash after its list let go of the old pages,
# before it freed them: the sweep frees them.  The crash keeps only the
# WAL written out before it, and the prune doesn't write its records out
# (the WAL writer, the background writer or a backend evicting the list's
# page might, or not yet): they are flushed before the crash.
my ($lineage, $e1) = split /\|/, sql(qq{SELECT lineage, epoch FROM test_bmap_fold_begin($rel)});
sql(q{UPDATE cow.t SET b = b + 1 WHERE a % 100 = 0});
my $e2 = sql(qq{SELECT epoch FROM test_bmap_fold_begin($rel)});
my $listed = sql(qq{SELECT count(DISTINCT old_block)
	FROM test_bmap_fold_changes($rel, $lineage, $e1) WHERE kind = 'copy'});
cmp_ok($listed, '>', 0, 'the changes list holds old pages');
$want = sql($sum);
my $p = $node->background_psql('sw', on_error_stop => 0);
$p->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-pruned', 'wait')});
$p->query_until(qr/started/, qq{
	\\echo started
	SELECT test_bmap_fold_prune($rel, $lineage, $e2);
});
$node->poll_query_until('sw', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-pruned'}) or die 'the prune did not stop';
my $pruned = sql('SELECT pg_current_wal_insert_lsn()');
sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
is(sql(qq{SELECT pg_current_wal_flush_lsn() >= '$pruned'}), 't',
	'the prune\'s records are flushed');
$node->stop('immediate');
eval { $p->quit; };
$node->start;
sql('CHECKPOINT');
is(sql($sweep), "$listed 0", 'a sweep frees the pages a crashed prune let go of');
is(sql($sweep), '0 0', 'and a second finds nothing');
is(sql($sum), $want, 'the table reads as before');

# A drop stopped by a crash after its commit, before its blocks were
# freed: they were queued before the commit record, so the sweep's drain
# frees them and the sweep finds none held by nothing.
sql(q{CREATE TABLE cow.d (a int8); INSERT INTO cow.d SELECT g FROM generate_series(1, 5000) g});
# (its change log's blocks are freed with it, and their maps' nodes)
my $dblocks = sql(q{SELECT (pg_relation_size('cow.d') + (pg_relation_size(('pg_toast.pg_prolly_log_' || 'cow.d'::regclass::oid)::regclass) + pg_relation_size(('pg_toast.pg_prolly_log_' || 'cow.d'::regclass::oid || '_index')::regclass))) / 8192});
$dblocks += sql(q{SELECT (SELECT count(DISTINCT block) FROM test_bmap_tree(pg_relation_filenode('cow.d'))) +
	(SELECT count(DISTINCT block) FROM test_bmap_tree(pg_relation_filenode(('pg_toast.pg_prolly_log_' || 'cow.d'::regclass::oid)::regclass))) +
	(SELECT count(DISTINCT block) FROM test_bmap_tree(pg_relation_filenode(('pg_toast.pg_prolly_log_' || 'cow.d'::regclass::oid || '_index')::regclass)))});
my $d = $node->background_psql('sw', on_error_stop => 0);
$d->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-predrop', 'wait')});
$d->query_until(qr/started/, q{
	\echo started
	DROP TABLE cow.d;
});
$node->poll_query_until('sw', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-predrop'}) or die 'the drop did not stop';
$node->stop('immediate');
eval { $d->quit; };
$node->start;
sql('CHECKPOINT');
cmp_ok(sql(q{SELECT test_bmap_release_pending()}), '>', 0,
	'a crash after a drop\'s commit leaves its blocks queued');
my $free_before = sql(q{SELECT test_bmap_free_blocks()});
is(sql(q{SELECT freed FROM test_bmap_release_drain()}), $dblocks,
	'a drain frees the blocks of a relation dropped by a crash');
my ($freed, $recounted) = split / /, sql($sweep);
is($freed, '0', 'and a sweep finds none held by nothing');
is(sql($sum), $want, 'the other table reads as before');

# Branch creation waits for a sweep counting the maps.
my $s = $node->background_psql('sw');
$s->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-sweep-counted', 'wait')});
$s->query_until(qr/started/, qq{
	\\echo started
	SELECT * FROM prolly_page_sweep();
	\\echo done
});
$node->poll_query_until('sw', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-sweep-counted'}) or die 'the sweep did not stop';
my $b = $node->background_psql('sw');
$b->query_until(qr/started/, qq{
	\\echo started
	SELECT test_bmap_share($rel) IS NOT NULL AS shared;
	\\echo done
});
$node->poll_query_until('sw', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event_type = 'Lock' AND query LIKE '%test_bmap_share%'})
  or die 'the share did not wait';
pass('a share waits for the sweep');

# Meanwhile another database's page store reuses its free blocks: only
# the store being swept holds off reuse.
$node->safe_psql('postgres', 'CREATE DATABASE sw2');
$node->safe_psql('sw2', q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.x (a int8);
	INSERT INTO cow.x SELECT g FROM generate_series(1, 5000) g;
	DROP TABLE cow.x;
	SELECT * FROM test_bmap_release_drain();
});
my $free2 = $node->safe_psql('sw2', 'SELECT test_bmap_free_blocks()');
cmp_ok($free2, '>', 0, 'the other store has free blocks');
$node->safe_psql('sw2', q{CREATE TABLE cow.y (a int8);
	INSERT INTO cow.y SELECT g FROM generate_series(1, 5000) g});
cmp_ok($node->safe_psql('sw2', 'SELECT test_bmap_free_blocks()'), '<', $free2,
	'and reuses them while this store is swept');
sql(q{SELECT injection_points_wakeup('bmap-sweep-counted')});
$s->query_until(qr/done/, '');
like($b->query_until(qr/done/, ''), qr/t/, 'and goes on after it');
$s->quit;
$b->quit;

# The standby follows: the same free blocks, the same store, the table.
sql('CHECKPOINT');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('sw', 'SELECT test_bmap_free_blocks()'),
	sql('SELECT test_bmap_free_blocks()'), 'the standby\'s allocator is the primary\'s');
my $store = q{SELECT (pg_stat_file(format('base/%s/9421',
	(SELECT oid FROM pg_database WHERE datname = 'sw')))).size};
is($standby->safe_psql('sw', $store), sql($store), 'and its store as long');
is($standby->safe_psql('sw', $sum), $want, 'and it reads the table');

$standby->stop;
$node->stop;
done_testing();
