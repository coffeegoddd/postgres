# Copyright (c) 2026, PostgreSQL Global Development Group

# pgvector's HNSW and IVFFlat indexes under roots, where pgvector is
# installed (skipped otherwise): a commit's root holds each index's pages
# as they were, so a hard reset to it gives them back, moving no row; an
# HNSW scan then finds what it found at the commit, and an IVFFlat scan
# probing every list just the rows a sequential scan does.  Writes after,
# VACUUM, a crash and a standby replay it all (generic WAL), every page
# checked against the primary's (wal_consistency_checking).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('root_vector_indexes');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_consistency_checking = 'all'
prolly.restore_min_changes = 0
});
$node->start;

if ($node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_available_extensions WHERE name = 'vector'}) eq '0')
{
	$node->stop;
	plan skip_all => 'pgvector is not installed';
}

$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('root_vector_indexes_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "wal_consistency_checking = 'all'");
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION vector; CREATE EXTENSION pg_buffercache;
	CREATE SCHEMA rv VERSIONED;
	CREATE TABLE rv.t (id int PRIMARY KEY, e vector(4) NOT NULL, f vector(4) NOT NULL)
		WITH (autovacuum_enabled = off);
	CREATE FUNCTION rv.vec(g int, k int) RETURNS vector LANGUAGE sql AS $$
		SELECT ('[' || (g % 17 + k) || ',' || (g % 13) || ',' || (g % 7) || ',' || (g / 50) || ']')::vector
	$$;
	INSERT INTO rv.t SELECT g, rv.vec(g, 0), rv.vec(g * 3, 1) FROM generate_series(1, 2000) g;
	CREATE INDEX t_hnsw ON rv.t USING hnsw (e vector_l2_ops) WITH (m = 8, ef_construction = 32);
	CREATE INDEX t_ivf ON rv.t USING ivfflat (f vector_l2_ops) WITH (lists = 8);
	SELECT dolt_commit('-Am', 'c0')});

my $digest = q{SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM rv.t t};
# an HNSW scan's nearest rows, and an IVFFlat scan's (every list probed),
# with a sequential scan's for it
my $hnsw = q{SET enable_seqscan = off; SET hnsw.ef_search = 100;
	SELECT string_agg(id::text, ',') FROM
	(SELECT id FROM rv.t ORDER BY e <-> '[3,4,2,10]' LIMIT 20) s};
my $ivf = q{SET enable_seqscan = off; SET ivfflat.probes = 8;
	SELECT string_agg(id::text, ',') FROM
	(SELECT id FROM rv.t ORDER BY f <-> '[5,1,3,20]', id LIMIT 20) s};
my $exact = q{SET enable_indexscan = off;
	SELECT string_agg(id::text, ',') FROM
	(SELECT id FROM rv.t ORDER BY f <-> '[5,1,3,20]', id LIMIT 20) s};
# each index's pages, as stored (its buffers written out and evicted)
sub pages
{
	my ($n) = @_;
	return $n->safe_psql('postgres', q{
		SELECT count(pg_buffercache_evict_relation(c.oid)) >= 0 FROM pg_class c
		WHERE relname IN ('t_hnsw', 't_ivf');
		SELECT string_agg(relname || ':' || d, ' ' ORDER BY relname) FROM
		(SELECT c.relname, md5(string_agg(md5(get_raw_page(c.oid::regclass::text, b::int)), '' ORDER BY b)) d
		 FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace,
			LATERAL generate_series(0, pg_relation_size(c.oid) / 8192 - 1) b
		 WHERE n.nspname = 'rv' AND c.relname IN ('t_hnsw', 't_ivf')
		 GROUP BY c.relname) s});
}
sql(q{CREATE EXTENSION pageinspect});

# c1: rows inserted, updated, deleted
sql(q{INSERT INTO rv.t SELECT g, rv.vec(g, 2), rv.vec(g, 3) FROM generate_series(3001, 3500) g;
	UPDATE rv.t SET e = rv.vec(id, 5), f = rv.vec(id, 6) WHERE id % 7 = 0;
	DELETE FROM rv.t WHERE id % 11 = 0;
	SELECT dolt_commit('-am', 'c1')});
my $at_c1 = sql($digest);
my $hnsw_c1 = sql($hnsw);
my $pages_c1 = pages($node);
is(sql($ivf), sql($exact), 'at c1, IVFFlat finds what a sequential scan does');

# c2: changed again, VACUUM removing entries from both
sql(q{UPDATE rv.t SET e = rv.vec(id, 9) WHERE id <= 1000;
	DELETE FROM rv.t WHERE id > 1500 AND id < 2000});
sql(q{VACUUM rv.t});
sql(q{SELECT dolt_commit('-am', 'c2')});

sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(sql($digest), $at_c1, 'a reset to c1 gives its rows');
is(pages($node), $pages_c1, 'and its indexes\' pages as captured');
is(sql($hnsw), $hnsw_c1, 'HNSW finds what it found at c1');
is(sql($ivf), sql($exact), 'IVFFlat finds what a sequential scan does');

# writes after the reset, VACUUM: the frozen root's dead line pointers'
# entries removed, the indexes found consistent
sql(q{INSERT INTO rv.t SELECT g, rv.vec(g, 4), rv.vec(g, 4) FROM generate_series(5001, 5400) g;
	UPDATE rv.t SET f = rv.vec(id, 8) WHERE id % 5 = 0;
	DELETE FROM rv.t WHERE id % 13 = 0});
sql(q{VACUUM rv.t});
is(sql($ivf), sql($exact), 'after writes and VACUUM, IVFFlat agrees');
my $at_c3 = sql($digest);
sql(q{SELECT dolt_commit('-am', 'c3')});

# a crash: redo replays the captures and the copies of their pages
$node->stop('immediate');
$node->start;
is(sql($digest), $at_c3, 'after a crash, the rows as committed');
is(sql($ivf), sql($exact), 'after a crash, IVFFlat agrees');
sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(sql($digest), $at_c1, 'after a crash, a reset to c1 gives its rows');
is(pages($node), $pages_c1, 'and its indexes\' pages as captured');
is(sql($hnsw), $hnsw_c1, 'HNSW finds what it found at c1');

$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $digest), $at_c1, 'the standby reads c1\'s rows');
is($standby->safe_psql('postgres', $hnsw), $hnsw_c1, 'and its HNSW scan finds the same');

$standby->stop;
$node->stop;
done_testing();
