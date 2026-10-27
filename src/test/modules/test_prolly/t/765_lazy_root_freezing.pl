# Copyright (c) 2026, PostgreSQL Global Development Group

# Under dolt.transaction_commit a commit's roots are captured among the
# writers and kept unfrozen, with the commit's snapshot, in the refs.  A
# crash between the capture and the freezing leaves them there: the fold
# launcher, starting, starts a worker in every database once, which
# freezes them.  Redo replays the captures and the freezing, page by page
# checked (wal_consistency_checking) on the primary and a standby; a
# reset gives the commit's rows back, frozen.  A commit whose roots fall
# past the database's frozen horizon before they are frozen is retired,
# never given back.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('lazy_root_freezing');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
prolly_fold_worker_naptime = 100
checkpoint_timeout = 1h
wal_consistency_checking = 'all'
prolly.restore_min_changes = 0
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('lazy_root_freezing_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $digest = q{SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM lr.t t};
my $frozen = q{SELECT count(*) FILTER (WHERE lp_flags = 1 AND (t_infomask & 768) <> 768)
	FROM generate_series(0, pg_relation_size('lr.t') / 8192 - 1) b,
		LATERAL heap_page_items(get_raw_page('lr.t', b::int))};
my $unfrozen = q{SELECT test_refs_value('umain') IS NOT NULL};
my $tc = q{SET dolt.transaction_commit = on; SET prolly.root_interval = 0;};

sql(q{CREATE EXTENSION pageinspect; CREATE EXTENSION amcheck;
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA lr VERSIONED;
	CREATE TABLE lr.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL);
	CREATE INDEX t_v ON lr.t (v);
	INSERT INTO lr.t SELECT g, 0, repeat('w', 40) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'base')});
sql('CHECKPOINT');

# two transactions' commits, their roots kept unfrozen
sql($tc . q{UPDATE lr.t SET v = 1 WHERE id % 7 = 0});
my $at_c1 = sql($digest);
sql($tc . q{DELETE FROM lr.t WHERE id % 11 = 0});
is(sql($unfrozen), 't', 'the commits\' roots kept unfrozen');

# a crash before they are frozen; the fold worker on as the server starts
$node->stop('immediate');
$node->append_conf('postgresql.conf', 'prolly_fold_worker = on');
$node->start;
ok($node->poll_query_until('postgres',
		q{SELECT test_refs_value('umain') IS NULL}),
	'after a crash, the fold worker freezes the roots left');

sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(sql($digest), $at_c1, 'a reset gives c1\'s rows');
is(sql($frozen), '0', 'every version left frozen');
my ($ret, $out, $err) = $node->psql('postgres',
	q{SELECT bt_index_check(indexrelid, true) FROM pg_index
	WHERE indrelid = 'lr.t'::regclass}, on_error_stop => 1);
is($ret, 0, 'the indexes valid');

$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $digest), $at_c1,
	'the standby replayed the captures and the freezing, and reads c1\'s rows');

# roots past the horizon before they are frozen: retired, not given back
$node->append_conf('postgresql.conf', 'prolly_fold_worker = off');
$node->reload;
sql($tc . q{UPDATE lr.t SET v = 2 WHERE id % 5 = 0});
my $at_c3 = sql($digest);
sql($tc . q{UPDATE lr.t SET v = 3 WHERE id % 3 = 0});
is(sql($unfrozen), 't', 'new commits\' roots kept unfrozen');
$node->safe_psql($_, 'VACUUM (FREEZE)')
	foreach (split /\n/, sql(q{SELECT datname FROM pg_database WHERE datallowconn}));
$node->append_conf('postgresql.conf', 'prolly_fold_worker = on');
$node->reload;
ok($node->poll_query_until('postgres',
		q{SELECT test_refs_value('umain') IS NULL}),
	'past the horizon, the roots are retired');
sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(sql($digest), $at_c3, 'a reset to the commit gives its rows, written');

$standby->stop;
$node->stop;
done_testing();
