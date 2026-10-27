# Copyright (c) 2026, PostgreSQL Global Development Group

# A streaming standby replaying shares and kept snapshots whose own blocks
# were logged as images: redo of each syncs nothing, the images' redo
# having written the blocks (#947); it writes out the relation's pages, as
# one may have changed in place after its image.  A block the standby's
# machine lost (zeroed) before a restartpoint synced it is rebuilt from
# the image after a crash; promoted, the standby gives each commit's kept
# pages back.  A base backup taken right after such a commit, the store
# never synced for it, restores its rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('wal_durable_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
wal_keep_size = 1GB
autovacuum = off
checkpoint_timeout = 1h
});
$primary->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }
my $contents = q{SELECT count(*), sum(v), sum(length(pad)) FROM app.t};

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text);
	INSERT INTO app.t SELECT i, 0, repeat('x', 300) FROM generate_series(1, 4000) i;
	SELECT dolt_commit('-Am', 'base');
});
$primary->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('wal_durable_standby');
$standby->init_from_backup($primary, 'b', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'log_min_messages = debug1');
$standby->start;
$primary->wait_for_replay_catchup($standby);
my $log_offset = -s $standby->logfile;

# Commits keeping pages changed since, and a first use of a branch (a
# share of the changed table).
my @at;
for my $round (1 .. 3)
{
	sql(qq{UPDATE app.t SET v = v + $round, pad = pad || '$round'
		WHERE id % (5 + $round) = 0;
		SELECT dolt_commit('-am', 'round $round')});
	push @at, sql($contents);
}
# (the blocks round 3 keeps)
my @lost = split /\n/, sql(q{
	SELECT DISTINCT m.phys FROM app.t t,
		test_bmap_map(pg_relation_filenode('app.t')) m
	WHERE t.id % 8 = 0 AND m.blkno = (t.ctid::text::point)[0]});
sql(q{UPDATE app.t SET v = v + 100 WHERE id % 3 = 0;
	SELECT dolt_branch('side'); SELECT dolt_checkout('side')});
# and a share of block-mapped storage holding blocks of its own
sql(q{SET debug_block_mapped_heap = on;
	CREATE TABLE plain (a int, b text);
	INSERT INTO plain SELECT g, repeat('p', 200) FROM generate_series(1, 2000) g;
	SELECT test_bmap_share(pg_relation_filenode('plain'))});
my $now = sql($contents);
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $contents), $now, 'the standby replayed them');
ok( $standby->log_contains(
		'redo of a snapshot of .* syncs nothing: its own pages were logged as images',
		$log_offset),
	'redo of a kept snapshot syncs nothing');
ok( $standby->log_contains('redo of a snapshot of .* writes out', $log_offset),
	'but writes out its pages');
ok( $standby->log_contains(
		'redo of a share of .* syncs nothing: its own pages were logged as images',
		$log_offset),
	'nor does redo of a share sync');

# The standby's machine loses the last commit's kept blocks (their
# restartpoint was before the commit), then the standby crashes.
my $store = $standby->data_dir . '/base/'
  . sql(q{SELECT oid FROM pg_database WHERE datname = current_database()})
  . '/9421';
ok(@lost > 0, 'round 3 changed pages: ' . scalar(@lost));
$standby->stop('immediate');
for my $phys (@lost)
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
	'after a crash and lost writes, the standby has the working set');

# Promoted, it gives the kept pages back on resets to each commit.
$standby->promote;
sub counted
{
	my ($name) = @_;
	return $standby->safe_psql('postgres', qq{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}
my $restored = counted('relations_restored');
$standby->safe_psql('postgres', q{SELECT dolt_reset('--hard')});
cmp_ok(counted('relations_restored'), '>', $restored,
	'the reset gave the kept pages back');
is($standby->safe_psql('postgres', $contents), $at[2],
	"and they hold the commit's rows");
my $hash = $standby->safe_psql('postgres',
	q{SELECT commit_hash FROM dolt_log WHERE message = 'round 2'});
$standby->safe_psql('postgres', qq{SELECT dolt_reset('--hard', '$hash')});
is($standby->safe_psql('postgres', $contents), $at[1],
	"a reset to round 2 has its commit's rows");
$standby->stop;

# A base backup taken while commits keep pages, none syncing a page store:
# restored, its last commit's kept pages hold that commit's rows.
sql(q{SELECT dolt_checkout('main'); SELECT dolt_reset('--hard');
	CREATE TABLE app.b (id int PRIMARY KEY, v int);
	INSERT INTO app.b SELECT g, 0 FROM generate_series(1, 1000) g;
	SELECT dolt_commit('-Am', 'b0')});
my $bg = $primary->background_psql('postgres');
$bg->query_until(qr/started/, "\\echo started\n" . join('', map {
	"UPDATE app.b SET v = $_ WHERE id % 10 = 0; SELECT dolt_commit('-am', 'b$_');\n" } 1 .. 300));
$primary->backup('during');
$bg->quit;
my $during = PostgreSQL::Test::Cluster->new('wal_durable_during');
$during->init_from_backup($primary, 'during');
$during->start;
my $last = $during->safe_psql('postgres',
	q{SELECT max(substr(message, 2)::int) FROM dolt_log WHERE message ~ '^b[0-9]+$'});
ok($last >= 0, "the backup has commits to b$last");
$during->safe_psql('postgres', q{UPDATE app.b SET v = -1; SELECT dolt_reset('--hard')});
is($during->safe_psql('postgres', q{SELECT sum(v) FROM app.b}), 100 * $last,
	'a backup taken during commits gives back its last commit\'s pages');
$during->stop;

# A base backup right after a commit that synced no page store.
sql(q{SELECT dolt_checkout('main'); SELECT dolt_reset('--hard');
	UPDATE app.t SET v = v - 1 WHERE id % 9 = 0;
	SELECT dolt_commit('-am', 'round 4')});
my $c4 = sql($contents);
$primary->backup('b4');
my $restored_node = PostgreSQL::Test::Cluster->new('wal_durable_restored');
$restored_node->init_from_backup($primary, 'b4');
$restored_node->start;
is($restored_node->safe_psql('postgres', $contents), $c4,
	'a base backup restores the commit\'s rows');
$restored_node->safe_psql('postgres',
	q{UPDATE app.t SET v = 0; SELECT dolt_reset('--hard')});
is($restored_node->safe_psql('postgres', $contents), $c4,
	'and a hard reset there gives back its kept pages');
$restored_node->stop;
$primary->stop;
done_testing();
