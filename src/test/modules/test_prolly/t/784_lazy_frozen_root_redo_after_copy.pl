# Copyright (c) 2026, PostgreSQL Global Development Group

# Under dolt.transaction_commit a commit's roots are captured among the
# writers and kept unfrozen, frozen later: by the fold worker, or by a
# move to the commit.  The freezing captures its frozen root of a copy, as
# a commit's own freezing does: temporary storage made of the relation's
# nearest frozen root, extended for the pages added since, written with
# the frozen pages, made a root of, and dropped as the transaction
# commits.  When a checkpoint's redo begins after the copy was made but
# before it was extended, crash recovery finds no map for the copy: only
# images naming the physical blocks, logged at the capture, give the
# frozen root its pages back.  After the crash the relation, made of the
# frozen root by the move, holds the commit's rows, and so does a reset
# to the commit again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('lazy_frozen_root_redo_after_copy');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
prolly.restore_min_changes = 0
prolly.kept_commits = 8
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $digest = q{SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM lf.t t};
my $tc = q{SET dolt.transaction_commit = on; SET prolly.root_interval = 0;};
sql(q{CREATE EXTENSION injection_points; CREATE EXTENSION test_prolly;
	CREATE SCHEMA lf VERSIONED;
	CREATE TABLE lf.t (id int PRIMARY KEY, w text NOT NULL)
		WITH (autovacuum_enabled = off);
	INSERT INTO lf.t SELECT g, repeat('w', 100) FROM generate_series(1, 1000) g;
	SELECT dolt_commit('-Am', 'base')});

# c1, a transaction's commit, grows the relation by many pages; it and c2
# are kept unfrozen
sql($tc . q{INSERT INTO lf.t SELECT g, repeat('g', 100) FROM generate_series(1001, 5000) g;
	UPDATE lf.t SET w = 'c1' WHERE id <= 10});
my $at_c1 = sql($digest);
my $c1_pages = sql(q{SELECT pg_relation_size('lf.t') / 8192});
sql($tc . q{UPDATE lf.t SET w = 'c2' WHERE id <= 2000});
is(sql(q{SELECT test_refs_value('umain') IS NOT NULL}), 't',
	'c1\'s and c2\'s roots kept unfrozen');

# a reset to c1 freezes its roots: it stops once its copy of the table is
# made, a checkpoint begins its redo there, then the copy is extended,
# written and captured
my $mover = $node->background_psql('postgres');
$mover->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('branch-keep-frozen-copy-made', 'wait')});
$mover->query_until(qr/started/, q{\echo started
SELECT dolt_reset('--hard', 'HEAD~1');
});
$node->wait_for_event('client backend', 'branch-keep-frozen-copy-made');
sql('CHECKPOINT');
sql(q{SELECT injection_points_detach('branch-keep-frozen-copy-made');
	SELECT injection_points_wakeup('branch-keep-frozen-copy-made')});
$mover->quit;
is(sql($digest), $at_c1, 'the reset gives c1\'s rows');

$node->stop('immediate');
$node->start;
is(sql(q{SELECT pg_relation_size('lf.t') / 8192}), $c1_pages, 'c1\'s length');
is(sql(q{SELECT count(*) FROM lf.t}), '5000',
	'after a crash, the relation made of c1\'s frozen root has every row');
is(sql($digest), $at_c1, 'and c1\'s rows as committed');

# and a reset to c1 again, made of the same frozen root
sql(q{UPDATE lf.t SET w = 'dirty' WHERE id <= 3000});
sql(q{SELECT dolt_reset('--hard')});
is(sql($digest), $at_c1, 'a reset to c1 again gives its rows');

$node->stop;
done_testing();
