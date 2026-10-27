# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit's frozen root is captured of a copy: temporary storage, made,
# extended for pages added since the last root, written with the frozen
# pages, made a root of, and dropped as the commit commits.  When a
# checkpoint's redo begins after the copy was made but before it was
# extended, crash recovery finds no map for the copy (dropped before the
# crash): its extension's redo zeroes the page store's blocks the root now
# holds, and its pages' records go to nothing the root holds.  Only images
# naming the physical blocks, logged at the capture, restore them; a reset
# to the commit then gives its rows, the added pages' among them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('frozen_root_redo_after_copy');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
prolly.restore_min_changes = 0
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('frozen_root_redo_after_copy_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $digest = q{SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM rf.t t};
sql(q{CREATE EXTENSION injection_points;
	CREATE SCHEMA rf VERSIONED;
	CREATE TABLE rf.t (id int PRIMARY KEY, w text NOT NULL)
		WITH (autovacuum_enabled = off);
	INSERT INTO rf.t SELECT g, repeat('w', 100) FROM generate_series(1, 1000) g;
	SELECT dolt_commit('-Am', 'base')});

# the relation grows by many pages; c1's commit stops once its copy of
# the table is made, a checkpoint begins its redo there, then the copy is
# extended, written and captured
sql(q{INSERT INTO rf.t SELECT g, repeat('g', 100) FROM generate_series(1001, 5000) g;
	UPDATE rf.t SET w = 'c1' WHERE id <= 10});
my $at_c1 = sql($digest);
my $c1_pages = sql(q{SELECT pg_relation_size('rf.t') / 8192});
my $committer = $node->background_psql('postgres');
$committer->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('branch-keep-frozen-copy-made', 'wait')});
$committer->query_until(qr/started/, q{\echo started
SELECT dolt_commit('-am', 'c1');
});
$node->wait_for_event('client backend', 'branch-keep-frozen-copy-made');
sql('CHECKPOINT');
# (the first relation's copy waits; the others' don't)
sql(q{SELECT injection_points_detach('branch-keep-frozen-copy-made');
	SELECT injection_points_wakeup('branch-keep-frozen-copy-made')});
$committer->quit;
sql(q{UPDATE rf.t SET w = 'c2' WHERE id <= 2000; SELECT dolt_commit('-am', 'c2')});
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

$node->stop('immediate');
$node->start;
sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(sql(q{SELECT pg_relation_size('rf.t') / 8192}), $c1_pages, 'c1\'s length');
is(sql(q{SELECT count(*) FROM rf.t}), '5000', 'after a crash, a reset to c1 gives every row');
is(sql($digest), $at_c1, 'and c1\'s rows as committed');

# the standby, which replayed it all with the copy's map there, agrees
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $digest), $at_c1, 'the standby reads c1\'s rows');

$standby->stop;
$node->stop;
done_testing();
