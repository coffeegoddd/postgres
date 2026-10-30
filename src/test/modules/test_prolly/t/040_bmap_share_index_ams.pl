# Copyright (c) 2026, PostgreSQL Global Development Group

# GiST, SP-GiST, GIN and BRIN indexes on block-mapped storage, shared with
# their table: rows written after the share on either side, inserted,
# updated and deleted, change only that side's index pages, which each
# copies before it first changes them.  Each side's indexes then find
# exactly its own rows (an index's answer equals a sequential scan's), and
# amcheck finds nothing wrong, after VACUUM, on a standby and after a
# crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('share_index_ams');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION amcheck;
	CREATE EXTENSION pg_walinspect;
});
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('share_index_ams_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }

# The table, with one index of each access method, and its rows.
sql(q{
	CREATE TABLE t (a int, p point, q point, arr int[], txt text);
	INSERT INTO t SELECT g, point(g % 1000, g / 20), point(g / 20, g % 1000),
		ARRAY[g % 50, g % 7, g], 'w' || (g % 300)
		FROM generate_series(1, 20000) g;
	CREATE INDEX t_gist ON t USING gist (p);
	CREATE INDEX t_spgist ON t USING spgist (q);
	CREATE INDEX t_gin ON t USING gin (arr) WITH (fastupdate = on);
	CREATE INDEX t_brin ON t USING brin (a) WITH (pages_per_range = 4);
	VACUUM t;
});

# The copy: a table like t whose storage, and its indexes', shares t's.
my @rels = ('t', 't_gist', 't_spgist', 't_gin', 't_brin');
my %copy;
for my $rel (@rels)
{
	my $node = sql(qq{SELECT pg_relation_filenode('$rel')});
	$copy{$rel} = sql(qq{SELECT test_bmap_share($node)});
}
sql(q{
	CREATE TABLE c (a int, p point, q point, arr int[], txt text);
	CREATE INDEX c_gist ON c USING gist (p);
	CREATE INDEX c_spgist ON c USING spgist (q);
	CREATE INDEX c_gin ON c USING gin (arr) WITH (fastupdate = on);
	CREATE INDEX c_brin ON c USING brin (a) WITH (pages_per_range = 4);
});
for my $rel (@rels)
{
	(my $c = $rel) =~ s/^t/c/;
	sql(qq{UPDATE pg_class SET relfilenode = $copy{$rel},
		relfrozenxid = (SELECT relfrozenxid FROM pg_class WHERE relname = '$rel'),
		relminmxid = (SELECT relminmxid FROM pg_class WHERE relname = '$rel')
		WHERE relname = '$c'});
}

# What each index answers, by an index (or bitmap) scan of 'index', and
# what a sequential scan answers.
my %probe = (
	gist => q{p <@ box '((100,100),(400,600))'},
	spgist => q{q <@ box '((100,100),(400,600))'},
	gin => q{arr @> ARRAY[7, 3]},
	brin => q{a BETWEEN 5000 AND 9000 OR a BETWEEN 26000 AND 28000},
);

sub answers
{
	my ($node, $table, $am) = @_;
	my $where = $probe{$am};
	my $q = qq{SELECT count(*) || ':' || coalesce(sum(a), 0) FROM $table WHERE $where};
	my $plan = $node->safe_psql('postgres', qq{
		SET enable_seqscan = off;
		EXPLAIN (COSTS OFF) $q});
	$plan =~ /${table}_$am/ or die "not a scan of ${table}_$am:\n$plan";
	my $by_index = $node->safe_psql('postgres', qq{SET enable_seqscan = off; $q});
	my $by_seq = $node->safe_psql('postgres', qq{
		SET enable_indexscan = off; SET enable_bitmapscan = off; $q});
	return ($by_index, $by_seq);
}

sub check_all
{
	my ($node, $what, %expect) = @_;

	for my $table ('t', 'c')
	{
		for my $am (sort keys %probe)
		{
			my ($by_index, $by_seq) = answers($node, $table, $am);
			is($by_index, $by_seq, "$what: $table\'s $am index finds its rows");
			is($by_seq, $expect{"$table.$am"}, "$what: and $table holds its own rows")
			  if exists $expect{"$table.$am"};
		}
		is($node->safe_psql('postgres', qq{SELECT gin_index_check('${table}_gin')}), '',
			"$what: amcheck finds $table\'s GIN index sound");
		is($node->safe_psql('postgres', qq{SELECT count(*) FROM verify_heapam('$table')}), '0',
			"$what: and its heap");
	}
}

my %before;
for my $am (sort keys %probe)
{
	my ($by_index, $by_seq) = answers($primary, 't', $am);
	$before{$am} = $by_seq;
}
check_all($primary, 'shared, unchanged',
	map { ("t.$_" => $before{$_}, "c.$_" => $before{$_}) } keys %probe);

# Reading copies nothing: every page of each relation is still shared.
for my $rel (@rels)
{
	my $node = sql(qq{SELECT pg_relation_filenode('$rel')});
	is(sql(qq{SELECT count(*) FILTER (WHERE s.phys <> c.phys) || '/' || count(*)
		FROM test_bmap_map($node) s JOIN test_bmap_map($copy{$rel}) c USING (blkno)}),
		'0/' . sql(qq{SELECT count(*) FROM test_bmap_map($node)}),
		"${rel}: reads leave every page shared");
}
my $lsn0 = sql(q{SELECT pg_current_wal_insert_lsn()});

# Every block of a relation or its copy that WAL says changed since the
# share no longer maps to the same block as the other side's: no change,
# by any path, reached a page both still share.
sub none_changed_shared
{
	my ($what) = @_;

	sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
	for my $rel (@rels)
	{
		my $node = sql(qq{SELECT pg_relation_filenode('$rel')});
		is(sql(qq{SELECT coalesce(string_agg(DISTINCT w.relfilenode || ':' || w.relblocknumber, ' '), '')
			FROM pg_get_wal_block_info('$lsn0', pg_current_wal_lsn()) w
			JOIN test_bmap_map($node) s ON s.blkno = w.relblocknumber
			JOIN test_bmap_map($copy{$rel}) c ON c.blkno = w.relblocknumber
			WHERE w.relfilenode IN ($node, $copy{$rel}) AND w.relforknumber = 0
			  AND s.phys = c.phys}), '',
			"$what: every block of $rel or its copy changed since the share is copied");
	}
}

# Each side writes its own rows: inserts that split pages (and fill GIN's
# pending list), updates that move rows, deletes.
sql(q{
	INSERT INTO t SELECT g, point(g % 1000, g / 20), point(g / 20, g % 1000),
		ARRAY[g % 50, 7, 3], 't' FROM generate_series(20001, 30000) g;
	UPDATE t SET p = point(150, 150), q = point(150, 150), arr = arr || 3
		WHERE a % 97 = 0;
	DELETE FROM t WHERE a % 13 = 0;
});
sql(q{
	INSERT INTO c SELECT g, point(g % 1000, g / 20), point(g / 20, g % 1000),
		ARRAY[g % 50, 7], 'c' FROM generate_series(40001, 52000) g;
	UPDATE c SET p = point(900, 900), q = point(900, 900), arr = ARRAY[7, 3]
		WHERE a % 89 = 0;
	DELETE FROM c WHERE a % 17 = 0;
});

my %after;
for my $table ('t', 'c')
{
	for my $am (sort keys %probe)
	{
		my ($by_index, $by_seq) = answers($primary, $table, $am);
		$after{"$table.$am"} = $by_seq;
	}
}
isnt($after{'t.gin'}, $after{'c.gin'}, 'the two sides hold different rows');
for my $rel (@rels)
{
	(my $c = $rel) =~ s/^t/c/;
	my $node = sql(qq{SELECT pg_relation_filenode('$rel')});
	is(sql(qq{SELECT count(*) > 0 FROM test_bmap_map($node) s
		JOIN test_bmap_map($copy{$rel}) c USING (blkno) WHERE s.phys <> c.phys}), 't',
		"${rel}'s changed pages were copied away from ${c}'s");
}
check_all($primary, 'written on both sides', %after);
none_changed_shared('written on both sides');

$primary->safe_psql('postgres', q{VACUUM t; VACUUM c});
check_all($primary, 'after VACUUM', %after);
none_changed_shared('after VACUUM');

# Index maintenance on the copy alone, after VACUUM (which summarizes into
# the revmap pages both still share): GIN's pending list cleaned, BRIN
# ranges summarized and one desummarized.  No row changes.
sql(q{
	SELECT gin_clean_pending_list('c_gin');
	SELECT brin_summarize_new_values('c_brin');
	SELECT brin_desummarize_range('c_brin', 0);
});
check_all($primary, 'after maintenance of the copy', %after);
none_changed_shared('after maintenance of the copy');

$primary->wait_for_replay_catchup($standby);
check_all($standby, 'on the standby', %after);

$primary->stop('immediate');
$primary->start;
check_all($primary, 'after a crash', %after);

done_testing();
