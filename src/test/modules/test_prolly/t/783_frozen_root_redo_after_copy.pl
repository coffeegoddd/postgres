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
#
# So too: with every record's redo checked (wal_consistency_checking) on
# the primary and a standby; on a server restored from a base backup taken
# while the copy waits; and with wal_level = minimal.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $digest = q{SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM rf.t t};

# c1's keep stopped once its copy of the table is made, while $during
# runs (a checkpoint begins its redo there), then let go: c1's rows and
# length
sub commit_c1
{
	my ($node, $during) = @_;
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };

	$sql->(q{CREATE EXTENSION injection_points;
		CREATE SCHEMA rf VERSIONED;
		CREATE TABLE rf.t (id int PRIMARY KEY, w text NOT NULL)
			WITH (autovacuum_enabled = off);
		INSERT INTO rf.t SELECT g, repeat('w', 100) FROM generate_series(1, 1000) g;
		SELECT dolt_commit('-Am', 'base')});

	# the relation grows by many pages before c1
	$sql->(q{INSERT INTO rf.t SELECT g, repeat('g', 100) FROM generate_series(1001, 5000) g;
		UPDATE rf.t SET w = 'c1' WHERE id <= 10});
	my $at_c1 = $sql->($digest);
	my $c1_pages = $sql->(q{SELECT pg_relation_size('rf.t') / 8192});
	my $committer = $node->background_psql('postgres');
	$committer->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('branch-keep-frozen-copy-made', 'wait')});
	$committer->query_until(qr/started/, q{\echo started
SELECT dolt_commit('-am', 'c1');
});
	$node->wait_for_event('client backend', 'branch-keep-frozen-copy-made');
	$sql->('CHECKPOINT');
	$during->() if $during;
	# (the first relation's copy waits; the others' don't)
	$sql->(q{SELECT injection_points_detach('branch-keep-frozen-copy-made');
		SELECT injection_points_wakeup('branch-keep-frozen-copy-made')});
	$committer->quit;
	$sql->(q{UPDATE rf.t SET w = 'c2' WHERE id <= 2000;
		SELECT dolt_commit('-am', 'c2')});
	return ($at_c1, $c1_pages);
}

# after a crash, a reset to c1 gives its rows
sub reset_to_c1
{
	my ($node, $what, $at_c1, $c1_pages) = @_;

	$node->safe_psql('postgres', q{SELECT dolt_reset('--hard', 'HEAD~1')});
	is($node->safe_psql('postgres', q{SELECT pg_relation_size('rf.t') / 8192}),
		$c1_pages, "$what: c1's length");
	is($node->safe_psql('postgres', q{SELECT count(*) FROM rf.t}), '5000',
		"$what: a reset to c1 gives every row");
	is($node->safe_psql('postgres', $digest), $at_c1,
		"$what: and c1's rows as committed");
}

my $conf = q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
prolly.restore_min_changes = 0
};

# streaming, every record's redo checked on the primary and a standby, and
# a base backup taken while the copy waits
{
	my $node = PostgreSQL::Test::Cluster->new('frozen_root_redo_after_copy');
	$node->init(allows_streaming => 1);
	$node->append_conf('postgresql.conf',
		$conf . "wal_consistency_checking = 'all'\nwal_keep_size = '1GB'\n");
	$node->start;
	$node->backup('base');
	my $standby = PostgreSQL::Test::Cluster->new('frozen_root_redo_after_copy_standby');
	$standby->init_from_backup($node, 'base', has_streaming => 1);
	$standby->start;

	my ($at_c1, $c1_pages) = commit_c1($node, sub { $node->backup('mid'); });
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

	# a server restored from the base backup taken mid-keep, its redo from
	# then on to c2, promoted
	my $restored = PostgreSQL::Test::Cluster->new('frozen_root_redo_after_copy_restored');
	$restored->init_from_backup($node, 'mid', has_streaming => 1);
	$restored->start;
	$node->wait_for_catchup($restored, 'replay', $node->lsn('insert'));
	$restored->promote;
	$restored->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	  or die 'the restored server never left recovery';
	$restored->safe_psql('postgres', q{SELECT dolt_reset('--hard', 'HEAD~1')});
	is($restored->safe_psql('postgres', q{SELECT count(*) FROM rf.t}), '5000',
		'restored from a base backup taken mid-keep: a reset to c1 gives every row');
	is($restored->safe_psql('postgres', $digest), $at_c1,
		'and c1\'s rows as committed');

	$restored->stop;

	$node->stop('immediate');
	$node->start;
	reset_to_c1($node, 'after a crash', $at_c1, $c1_pages);

	# the standby, which replayed it all with the copy's map there, agrees
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
	is($standby->safe_psql('postgres', $digest), $at_c1,
		'the standby reads c1\'s rows');

	$standby->stop;
	$node->stop;
}

# wal_level = minimal: the copy's storage made in the keeping transaction
{
	my $node = PostgreSQL::Test::Cluster->new('frozen_root_redo_after_copy_minimal');
	$node->init;
	$node->append_conf('postgresql.conf',
		$conf . "wal_level = minimal\nmax_wal_senders = 0\n");
	$node->start;
	my ($at_c1, $c1_pages) = commit_c1($node);
	$node->stop('immediate');
	$node->start;
	reset_to_c1($node, 'wal_level = minimal, after a crash', $at_c1, $c1_pages);
	$node->stop;
}

done_testing();
