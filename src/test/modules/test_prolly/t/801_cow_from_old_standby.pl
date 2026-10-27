# Copyright (c) 2026, PostgreSQL Global Development Group

# Copies of shared pages logged without their images (their redo copying
# the old block, which the capture's image wrote), replayed by a streaming
# standby, every record checked as it is replayed
# (wal_consistency_checking): the standby has the working set, and, its
# machine losing the old blocks before a crash, still has it after; a base
# backup taken between a capture and the change, or in the middle of a
# capture, restores both; promoted, the standby copies with images (its
# pages carry no capture's stamp) and recovers from a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $contents = q{SELECT count(*), sum(v), sum(length(pad)) FROM app.t};
my $checked = q{SELECT bool_and(bt_index_check(indexrelid, true) IS NOT NULL)
	FROM pg_index WHERE indrelid = 'app.t'::regclass};

my $primary = PostgreSQL::Test::Cluster->new('cow_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
wal_keep_size = 1GB
autovacuum = off
# (a page's stamp is its buffer's: the pages are to stay cached, and no
# checkpoint is to come between the cycles)
shared_buffers = 32MB
max_wal_size = 1GB
checkpoint_timeout = 1h
wal_consistency_checking = 'all'
});
$primary->start;
sub sql { return $primary->safe_psql('postgres', $_[0]); }
sub counted
{
	my ($node, $name) = @_;
	return $node->safe_psql('postgres', qq{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}
my $injection = $ENV{enable_injection_points} eq 'yes';

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION amcheck;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int NOT NULL, pad text NOT NULL);
	CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT i, i, repeat('x', 200) FROM generate_series(1, 6000) i;
	SELECT dolt_commit('-Am', 'base');
});
sql('CREATE EXTENSION injection_points') if $injection;
$primary->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('cow_standby');
$standby->init_from_backup($primary, 'b', has_streaming => 1);
$standby->start;

# The cycles: each a change of spread rows' indexed column, then a commit.
my $imageless = counted($primary, 'bmap_copies_imageless');
my %before;
for my $round (1 .. 3)
{
	%before = map { $_ => 1 } split /\n/, sql(q{
		SELECT m.phys FROM pg_index i,
			test_bmap_map(pg_relation_filenode(i.indexrelid)) m
		WHERE i.indrelid = 'app.t'::regclass});
	sql(qq{UPDATE app.t SET v = v - $round WHERE id % 40 = 0});
	sql(qq{SELECT dolt_commit('-am', 'round $round')});
}
sql(q{UPDATE app.t SET v = v - 10 WHERE id % 40 = 0});
cmp_ok(counted($primary, 'bmap_copies_imageless'), '>', $imageless,
	'copies without their images');
my $now = sql($contents);
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $contents), $now, 'the standby replayed them');
is($standby->safe_psql('postgres', $checked), 't', 'its indexes are valid');

# The standby's machine loses the old blocks the last copies left, then
# crashes: its replay from its restartpoint writes them again first.
my %after = map { $_ => 1 } split /\n/, sql(q{
	SELECT m.phys FROM pg_index i,
		test_bmap_map(pg_relation_filenode(i.indexrelid)) m
	WHERE i.indrelid = 'app.t'::regclass});
my @old = grep { !$after{$_} } keys %before;
ok(@old > 0, 'the copies left old blocks: ' . scalar(@old));
my $store = $standby->data_dir . '/base/'
  . sql(q{SELECT oid FROM pg_database WHERE datname = current_database()})
  . '/9421';
$standby->stop('immediate');
for my $phys (@old)
{
	my $seg = int($phys / 131072);
	my $path = $seg ? "$store.$seg" : $store;
	open(my $fh, '+<:raw', $path) or die "open $path: $!";
	seek($fh, ($phys % 131072) * 8192, 0) or die "seek $path: $!";
	print $fh ("\0" x 8192);
	close($fh) or die "close $path: $!";
}
$standby->start;
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $contents), $now,
	'after a crash and lost old blocks, the standby has the working set');
is($standby->safe_psql('postgres', $checked), 't', 'and valid indexes');

# A base backup between a capture and the change: its checkpoint is past
# the capture, so the change's copies log their images.
sql(q{SELECT dolt_commit('-am', 'before backup')});
$primary->backup('between');
$imageless = counted($primary, 'bmap_copies_imageless');
sql(q{UPDATE app.t SET v = v - 20 WHERE id % 40 = 0});
is(counted($primary, 'bmap_copies_imageless'), $imageless,
	'copies after a base backup began log their images');
$now = sql($contents);
my $between = PostgreSQL::Test::Cluster->new('cow_between');
$between->init_from_backup($primary, 'between', has_streaming => 1);
$between->start;
$primary->wait_for_replay_catchup($between);
is($between->safe_psql('postgres', $contents), $now,
	'a base backup taken between a capture and the change has the working set');
is($between->safe_psql('postgres', $checked), 't', 'and valid indexes');
$between->stop;

# A base backup in the middle of a capture: its pages logged, the capture's
# record not yet in.
if ($injection)
{
	my $w = $primary->background_psql('postgres');
	$w->query_safe(q{SELECT injection_points_set_local()});
	$w->query_safe(q{SELECT injection_points_attach('bmap-root-preserved', 'wait')});
	$w->query_until(qr/capturing/, q{\echo capturing
SELECT dolt_commit('-am', 'mid');
});
	$primary->wait_for_event('client backend', 'bmap-root-preserved');

	# (the backup's checkpoint may wait for a page the capture holds: it
	# runs in the background, and the capture is let go once the
	# checkpoint has set its redo pointer)
	my $redo = sql(q{SELECT redo_lsn FROM pg_control_checkpoint()});
	my $dir = $primary->backup_dir . '/mid';
	my $bb = IPC::Run::start(
		[ 'pg_basebackup', '--pgdata' => $dir, '--host' => $primary->host,
		  '--port' => $primary->port, '--checkpoint' => 'fast', '--no-sync' ],
		'>' => \my $bb_out, '2>' => \my $bb_err);
	my $began = 0;
	for (1 .. 300)
	{
		$began = sql(qq{SELECT EXISTS (SELECT FROM pg_stat_activity
				WHERE backend_type = 'checkpointer' AND wait_event LIKE 'Buffer%')
			OR (SELECT redo_lsn FROM pg_control_checkpoint()) <> '$redo'}) eq 't';
		last if $began;
		usleep(100_000);
	}
	ok($began, "the backup's checkpoint began during the capture");
	sql(q{SELECT injection_points_detach('bmap-root-preserved');
		SELECT injection_points_wakeup('bmap-root-preserved')});
	$bb->finish or die "pg_basebackup failed: $bb_err";
	$w->quit;
	sql(q{UPDATE app.t SET v = v - 30 WHERE id % 40 = 0});
	$now = sql($contents);
	my $mid = PostgreSQL::Test::Cluster->new('cow_mid');
	$mid->init_from_backup($primary, 'mid', has_streaming => 1);
	$mid->start;
	$primary->wait_for_replay_catchup($mid);
	is($mid->safe_psql('postgres', $contents), $now,
		'a base backup taken in the middle of a capture has the working set');
	is($mid->safe_psql('postgres', $checked), 't', 'and valid indexes');
	$mid->stop;
}

# Promoted, the standby copies with images, and recovers from a crash.
sql(q{SELECT dolt_commit('-am', 'last')});
my $last = sql($contents);
$primary->wait_for_replay_catchup($standby);
$standby->promote;
$standby->safe_psql('postgres', q{UPDATE app.t SET v = v - 40 WHERE id % 40 = 0});
my $promoted = $standby->safe_psql('postgres', $contents);
$standby->stop('immediate');
$standby->start;
is($standby->safe_psql('postgres', $contents), $promoted,
	'promoted, a crash after copies recovers the working set');
is($standby->safe_psql('postgres', $checked), 't', 'and valid indexes');
$standby->safe_psql('postgres', q{SELECT dolt_reset('--hard')});
is($standby->safe_psql('postgres', $contents), $last,
	"a reset gives the last commit's rows back");
$standby->stop;
$primary->stop;
done_testing();
