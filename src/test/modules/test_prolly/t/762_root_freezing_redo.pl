# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit's roots hold its heap pages frozen, each written to a copy of
# the relation's last root as an image and the copy made a root.  Redo
# replays it all: after a crash, and on a standby, every page checked
# against the primary's (wal_consistency_checking), a reset gives the
# commit's rows back, frozen, and the standby reads them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('root_freezing_redo');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_consistency_checking = 'all'
prolly.restore_min_changes = 0
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('root_freezing_redo_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "wal_consistency_checking = 'all'");
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $digest = q{SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM rf.t t};
my $frozen = q{SELECT count(*) FILTER (WHERE lp_flags = 1 AND (t_infomask & 768) <> 768)
	FROM generate_series(0, pg_relation_size('rf.t') / 8192 - 1) b,
		LATERAL heap_page_items(get_raw_page('rf.t', b::int))};
sql(q{CREATE EXTENSION pageinspect; CREATE EXTENSION amcheck;
	CREATE SCHEMA rf VERSIONED;
	CREATE TABLE rf.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL)
		WITH (fillfactor = 60);
	CREATE INDEX t_v ON rf.t (v);
	INSERT INTO rf.t SELECT g, g % 10, repeat('w', 40) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'base')});
sql('CHECKPOINT');

# HOT and non-HOT updates, deletes, an aborted transaction's versions
sql(q{UPDATE rf.t SET w = 'hot' || id WHERE id % 7 = 0;
	UPDATE rf.t SET v = v + 100 WHERE id % 11 = 0;
	DELETE FROM rf.t WHERE id % 13 = 0});
sql(q{BEGIN; UPDATE rf.t SET w = 'aborted' WHERE id % 17 = 0; ROLLBACK});
my $at_c1 = sql($digest);
sql(q{SELECT dolt_commit('-am', 'c1')});
sql(q{UPDATE rf.t SET w = 'later' WHERE id <= 1500; SELECT dolt_commit('-am', 'c2')});

# a crash: recovery replays the frozen captures
$node->stop('immediate');
$node->start;
sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(sql($digest), $at_c1, 'after a crash, a reset gives c1\'s rows');
is(sql($frozen), '0', 'every version left frozen');
is(sql(q{SELECT bool_and(bt_index_check(indexrelid, true) IS NOT NULL)
	FROM pg_index WHERE indrelid = 'rf.t'::regclass}), 't', 'the indexes valid');

# the standby replayed it all, page by page checked, and reads c1's rows
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $digest), $at_c1, 'the standby reads c1\'s rows');
is($standby->safe_psql('postgres', 'SELECT 1'), '1', 'and is running');

$standby->stop;
$node->stop;
done_testing();
