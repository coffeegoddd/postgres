# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit's pages kept by a snapshot (a root) are marked
# shared in the buffers and written out as it's taken, one look at the
# buffers for all its relations: each block looked up when they're few
# beside the buffers, else a pass over them.  Both ways, a page changed
# after it is copied first, so a hard reset that gives the pages back
# reads the commit's rows.  Here the buffers are few (128), so every
# relation's blocks are many beside them and the pass is taken; the
# regression tests' buffers take the lookups.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_pages_buffers');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_buffers = 1MB
autovacuum = off
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v int, pad text);
	INSERT INTO s.t SELECT g, 0, repeat('x', 20) FROM generate_series(1, 200) g;
	SELECT dolt_commit('-Am', 'base');
});
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM s.t};

for my $round (1 .. 3)
{
	# (the pages of a commit kept; rows changed in buffers then)
	sql(qq{UPDATE s.t SET v = $round WHERE id % 7 = 0});
	sql(qq{SELECT dolt_commit('-am', 'round $round')});
	my $at = sql($digest);
	sql(q{SELECT pg_stat_reset_shared('prolly')});
	sql(q{UPDATE s.t SET v = -1 WHERE id % 5 = 0});
	sql(q{UPDATE s.t SET v = -2 WHERE id % 3 = 0});
	sql(q{SELECT dolt_reset('--hard')});
	is(sql(q{SELECT pg_stat_force_next_flush();
		SELECT value > 0 FROM pg_stat_prolly WHERE name = 'relations_restored'}),
		"\nt", "round $round: the pages kept given back");
	is(sql($digest), $at, "round $round: the commit's rows");
	is(sql(q{SELECT count(*) FROM dolt_status}), '0', "round $round: clean");
}

done_testing();
