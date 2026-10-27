# Copyright (c) 2026, PostgreSQL Global Development Group

# Copies of shared pages logged without their images: a capture logs each
# changed page's image and stamps those unchanged since, and each one's
# first change after copies it to a new block with no image, redo copying
# the old block, which the capture's image wrote in every replay that
# reaches the copy.  Under every WAL level and with full_page_writes off,
# with every record checked as it is replayed (wal_consistency_checking):
#
# - a crash after such copies, the old blocks lost by then (zeroed: freed
#   and given another relation's page, or never synced), recovers the
#   working set and the commit's kept pages;
# - a checkpoint between the capture and the change logs the copies'
#   images, so a crash then, the old blocks lost, recovers the working set;
# - a checkpoint that begins as a copy is made, its redo pointer past the
#   capture, has the copy's image follow it (bmap_copy_images_late), so a
#   replay from that checkpoint, the old block lost, has the page;
# - with full_page_writes off, copies log their images.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $contents = q{SELECT count(*), sum(v), sum(length(pad)) FROM app.t};
my $checked = q{SELECT bool_and(bt_index_check(indexrelid, true) IS NOT NULL)
	FROM pg_index WHERE indrelid = 'app.t'::regclass};

sub counted
{
	my ($node, $name) = @_;
	return $node->safe_psql('postgres', qq{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}

# The physical blocks of the table's indexes, as their maps name them now.
sub index_blocks
{
	my ($node) = @_;
	return {
		map { $_ => 1 } split /\n/, $node->safe_psql('postgres', q{
		SELECT m.phys FROM pg_index i,
			test_bmap_map(pg_relation_filenode(i.indexrelid)) m
		WHERE i.indrelid = 'app.t'::regclass})
	};
}

# Zero the page store's blocks of $before that the maps no longer name:
# the old blocks the copies since left.
sub lose_old_blocks
{
	my ($node, $before) = @_;
	my $after = index_blocks($node);
	my @old = grep { !$after->{$_} } keys %$before;
	my $store = $node->data_dir . '/base/'
	  . $node->safe_psql('postgres',
		q{SELECT oid FROM pg_database WHERE datname = current_database()})
	  . '/9421';
	$node->stop('immediate');
	for my $phys (@old)
	{
		my $seg = int($phys / 131072);
		my $path = $seg ? "$store.$seg" : $store;
		open(my $fh, '+<:raw', $path) or die "open $path: $!";
		seek($fh, ($phys % 131072) * 8192, 0) or die "seek $path: $!";
		print $fh ("\0" x 8192);
		close($fh) or die "close $path: $!";
	}
	$node->start;
	return scalar(@old);
}

sub run_case
{
	my ($name, $conf, $race, $fpw) = @_;
	my $node = PostgreSQL::Test::Cluster->new("cow_$name");
	$node->init;
	$node->append_conf('postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 1GB
wal_consistency_checking = 'all'
$conf});
	$node->start;
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };
	$sql->('CREATE EXTENSION injection_points') if $race;

	$sql->(q{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION amcheck;
		CREATE SCHEMA app VERSIONED;
		CREATE TABLE app.t (id int PRIMARY KEY, v int NOT NULL, pad text NOT NULL);
		CREATE INDEX t_v ON app.t (v);
		INSERT INTO app.t SELECT i, i, repeat('x', 200) FROM generate_series(1, 6000) i;
		SELECT dolt_commit('-Am', 'base');
		UPDATE app.t SET v = -v WHERE id % 40 = 0;
		SELECT dolt_commit('-am', 'one');
	});
	my $one = $sql->($contents);

	# copies without images, then a crash with their old blocks lost
	my $before = index_blocks($node);
	my $imageless = counted($node, 'bmap_copies_imageless');
	$sql->(q{UPDATE app.t SET v = v - 1 WHERE id % 40 = 0});
	my $now = $sql->($contents);
	if ($fpw)
	{
		cmp_ok(counted($node, 'bmap_copies_imageless'), '>', $imageless,
			"$name: the indexes' pages were copied without their images");
	}
	else
	{
		is(counted($node, 'bmap_copies_imageless'), $imageless,
			"$name: without full-page writes, copies log their images");
	}
	cmp_ok(lose_old_blocks($node, $before), '>', 0,
		"$name: the old blocks lost before a crash");
	is($sql->($contents), $now, "$name: recovery has the working set");
	is($sql->($checked), 't', "$name: and its indexes are valid");
	$sql->(q{SELECT dolt_reset('--hard')});
	is($sql->($contents), $one, "$name: the commit's kept pages give its rows back");
	is($sql->($checked), 't', "$name: and valid indexes");

	if ($race)
	{
		# a checkpoint begun as a copy is made
		$sql->(q{UPDATE app.t SET v = v - 2 WHERE id % 40 = 0;
			SELECT dolt_commit('-am', 'race')});
		$before = index_blocks($node);
		my $late = counted($node, 'bmap_copy_images_late');
		my $w = $node->background_psql('postgres');
		$w->query_safe(q{SELECT injection_points_set_local()});
		$w->query_safe(
			q{SELECT injection_points_attach('bmap-copy-from-old', 'wait')});
		$w->query_until(qr/copying/, q{\echo copying
UPDATE app.t SET v = v - 3 WHERE id % 40 = 0;
});
		$node->wait_for_event('client backend', 'bmap-copy-from-old');
		my $redo = $sql->(q{SELECT redo_lsn FROM pg_control_checkpoint()});
		my $c = $node->background_psql('postgres');
		$c->query_until(qr/checkpointing/, q{\echo checkpointing
CHECKPOINT;
});
		# (the checkpoint's redo pointer is set before it writes buffers:
		# once it waits for one, or is done, it is past the capture)
		my $began = 0;
		for (1 .. 300)
		{
			$began = $sql->(qq{SELECT EXISTS (SELECT FROM pg_stat_activity
				WHERE backend_type = 'checkpointer' AND wait_event LIKE 'Buffer%')
			OR (SELECT redo_lsn FROM pg_control_checkpoint()) <> '$redo'}) eq 't';
			last if $began;
			usleep(100_000);
		}
		ok($began, "$name: the checkpoint began as the copy waited");
		$sql->(q{SELECT injection_points_detach('bmap-copy-from-old');
			SELECT injection_points_wakeup('bmap-copy-from-old')});
		$w->quit;
		$c->quit;
		$now = $sql->($contents);
		cmp_ok(counted($node, 'bmap_copy_images_late'), '>', $late,
			"$name: a copy made as a checkpoint began was imaged after all");
		lose_old_blocks($node, $before);
		is($sql->($contents), $now,
			"$name: a replay from that checkpoint, the old blocks lost, has the working set");
		is($sql->($checked), 't', "$name: and its indexes are valid");
	}

	# a checkpoint between the capture and the change: images logged
	$sql->(q{UPDATE app.t SET v = v - 4 WHERE id % 40 = 0;
		SELECT dolt_commit('-am', 'two'); CHECKPOINT});
	$before = index_blocks($node);
	$imageless = counted($node, 'bmap_copies_imageless');
	$sql->(q{UPDATE app.t SET v = v - 5 WHERE id % 40 = 0});
	$now = $sql->($contents);
	is(counted($node, 'bmap_copies_imageless'), $imageless,
		"$name: copies after a checkpoint past the capture log their images");
	lose_old_blocks($node, $before);
	is($sql->($contents), $now,
		"$name: a crash then, the old blocks lost, recovers the working set");
	is($sql->($checked), 't', "$name: and its indexes are valid");
	$node->stop;
}

run_case('replica', '', $ENV{enable_injection_points} eq 'yes', 1);
run_case('minimal', "wal_level = minimal\nmax_wal_senders = 0\n", 0, 1);
run_case('fpw_off', "full_page_writes = off\n", 0, 0);
done_testing();
