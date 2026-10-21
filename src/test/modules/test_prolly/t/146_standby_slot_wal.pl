# Copyright (c) 2026, PostgreSQL Global Development Group

# What a standby's logical slots' position, kept in its physical slot on the
# primary (standby_restart_lsn in pg_replication_slots), does on the
# primary, and how a slot's creation on a standby waits for it to be kept.
#
# The physical slot keeps the WAL from that position, as a logical slot
# keeps its own, so collections can scan it for the values the standby's
# slots may still decode.  Past max_slot_wal_keep_size, the WAL goes: a
# collection then warns and scans what is left, collecting what only the
# rest named, rather than keeping everything; once the standby's slot moves
# on, collections are quiet again.  A collection's WAL scan that fails
# leaves no file open.  A slot's creation waits for a collection that runs
# longer than wal_receiver_timeout, the primary saying the request is
# pending; it fails, saying why, if the primary doesn't answer, or if the
# standby is promoted meanwhile.  A slot file of the version before
# standby_restart_lsn is read, with none.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('slot_wal_primary');
$node->init(allows_streaming => 'logical', extra => ['--wal-segsize=1']);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
min_wal_size = 2MB
max_wal_size = 8MB
});
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sub value
{
	return "(SELECT string_agg(md5('$_[0]' || i), '' ORDER BY i)
		FROM generate_series(1, 3000) i)";
}
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }
sub gc { return sql('SELECT test_chunk_gc(true) IS NOT NULL'); }
sub root
{
	return sql("SELECT (test_toast_pointer(t)).root FROM d.docs WHERE id = $_[0]");
}
# WAL enough for a few segments, each switched and checkpointed away
sub churn
{
	for (1 .. 6)
	{
		sql(q{INSERT INTO d.pad SELECT g, repeat('p', 200)
			FROM generate_series(1, 2000) g});
		sql('DELETE FROM d.pad');
		sql('SELECT pg_switch_wal()');
		sql('CHECKPOINT');
	}
}
sub standby_lsn
{
	return sql(q{SELECT standby_restart_lsn FROM pg_replication_slots
		WHERE slot_name = 'sb'});
}

sql(q{SELECT pg_create_physical_replication_slot('sb')});
sql('CREATE EXTENSION test_prolly');
sql('CREATE SCHEMA d VERSIONED');
sql('CREATE TABLE d.docs (id int PRIMARY KEY, t text)');
sql(q{INSERT INTO d.docs VALUES (0, 'stays')});
sql('CREATE TABLE d.pad (id int, t text)');

$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('slot_wal_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
primary_slot_name = 'sb'
hot_standby_feedback = off
wal_receiver_status_interval = 1
});
$standby->start;
$node->wait_for_replay_catchup($standby);

is(standby_lsn(), '', 'with no logical slot on the standby, no position');
$standby->create_logical_slot_on_standby($node, 'ss', 'postgres');
my $pos = $standby->safe_psql('postgres',
	q{SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = 'ss'});
is(standby_lsn(), $pos,
	'the physical slot shows the standby\'s logical slot\'s position');

# A value named after the position, its row gone.
sql('INSERT INTO d.docs VALUES (1, ' . value('early') . ')');
my $early = root(1);
sql('DELETE FROM d.docs WHERE id = 1');
sql('VACUUM d.docs');

# The WAL from the position is kept, however much comes after.
churn();
$node->wait_for_replay_catchup($standby);
is( sql(qq{SELECT count(*) FROM pg_ls_waldir()
		WHERE name = pg_walfile_name('$pos')}),
	'1',
	'the primary keeps the WAL from the standby\'s position');
my $offset = -s $node->logfile;
gc();
is(has($early), 't', 'and a collection keeps the value it names');
ok(!$node->log_contains('which a logical replication slot may still decode, is gone', $offset),
	'without a warning');

# Past max_slot_wal_keep_size, the WAL goes; the physical slot stays.
$node->append_conf('postgresql.conf', 'max_slot_wal_keep_size = 2MB');
$node->reload;
churn();
$node->wait_for_replay_catchup($standby);
is( sql(qq{SELECT count(*) FROM pg_ls_waldir()
		WHERE name = pg_walfile_name('$pos')}),
	'0',
	'past max_slot_wal_keep_size, the WAL from the position goes');
is( sql(q{SELECT invalidation_reason IS NULL FROM pg_replication_slots
		WHERE slot_name = 'sb'}),
	't',
	'and the physical slot stays valid');
$offset = -s $node->logfile;
gc();
ok($node->log_contains('which a logical replication slot may still decode, is gone', $offset),
	'a collection warns');
is(has($early), 'f',
	'and collects what only the WAL that went named, rather than keeping everything');

# The standby's slot moves on: collections are quiet again.  (Its position
# moves as decoding passes running-xacts records.)
for (1 .. 2)
{
	sql('SELECT pg_log_standby_snapshot()');
	$node->wait_for_replay_catchup($standby);
	$standby->safe_psql('postgres',
		q{SELECT pg_replication_slot_advance('ss', pg_last_wal_replay_lsn())});
}
my $moved = $standby->safe_psql('postgres',
	q{SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = 'ss'});
ok( $node->poll_query_until(
		'postgres', qq{SELECT standby_restart_lsn >= '$moved'
		FROM pg_replication_slots WHERE slot_name = 'sb'}),
	'the position moves on with the standby\'s slot');
$offset = -s $node->logfile;
gc();
ok(!$node->log_contains('which a logical replication slot may still decode, is gone', $offset),
	'and a collection no longer warns');

SKIP:
{
	skip 'Injection points not supported by this build', 9
	  unless $ENV{enable_injection_points} eq 'yes';
	sql('CREATE EXTENSION injection_points');

	# A WAL scan that fails closes the segment it had open.
	SKIP:
	{
		skip 'no /proc to count open files', 1 unless -d '/proc/self/fd';
		my $s = $node->background_psql('postgres', on_error_stop => 0);
		my $pid = $s->query_safe('SELECT pg_backend_pid()');
		$s->query_safe(q{SELECT injection_points_set_local();
			SELECT injection_points_attach('prolly-wal-roots-read', 'error')});
		# (the WAL segments open: the one it writes, and any it reads)
		my $fds = sub {
			opendir(my $d, "/proc/$pid/fd") or die "/proc/$pid/fd: $!";
			my $n = grep { (readlink("/proc/$pid/fd/$_") // '') =~ m{/pg_wal/} }
			  grep { !/^\./ } readdir($d);
			closedir($d);
			return $n;
		};
		$s->query('SELECT test_chunk_gc(true) IS NOT NULL');
		my $before = $fds->();
		$s->query('SELECT test_chunk_gc(true) IS NOT NULL') for 1 .. 3;
		is($fds->(), $before,
			'a collection whose WAL scan fails leaves no file open');
		$s->quit;
	}

	# A collection that outlasts the standby's wal_receiver_timeout, which
	# read the positions before a slot's creation: the primary says the
	# request is pending, and the creation waits for it.
	$standby->append_conf('postgresql.conf', 'wal_receiver_timeout = 2s');
	$standby->reload;
	my $g = $node->background_psql('postgres');
	$g->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-gc-roots-scanned', 'wait')});
	$g->query_until(qr/collecting/, q{
		\echo collecting
		SELECT test_chunk_gc(true) IS NOT NULL;
		\echo collected
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0
		FROM pg_stat_activity WHERE wait_event = 'prolly-gc-roots-scanned'})
	  or die 'the collection did not stop';
	my ($out, $err) = ('', '');
	my $create = IPC::Run::start(
		[
			'pg_recvlogical',
			'--dbname' => $standby->connstr('postgres'),
			'--plugin' => 'test_decoding',
			'--slot' => 'sl',
			'--create-slot'
		],
		'>' => \$out,
		'2>' => \$err);
	ok( $standby->poll_query_until(
			'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
			WHERE wait_event = 'ReplicationSlotUpstreamHorizon'}),
		'a slot\'s creation waits for the running collection');
	sleep(5);
	is( $standby->safe_psql(
			'postgres', q{SELECT count(*) FROM pg_stat_activity
			WHERE wait_event = 'ReplicationSlotUpstreamHorizon'}),
		'1',
		'past wal_receiver_timeout, still');
	sql(q{SELECT injection_points_wakeup('prolly-gc-roots-scanned')});
	$g->query_until(qr/collected/, '');
	$g->quit;
	ok( $standby->poll_query_until(
			'postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
			WHERE wait_event = 'ReplicationSlotUpstreamHorizon'}),
		'and goes on once the collection is done');
	sql('SELECT pg_log_standby_snapshot()');
	$create->finish();
	is($standby->slot('sl')->{'slot_type'}, 'logical', 'the slot is created');
	$standby->safe_psql('postgres', q{SELECT pg_drop_replication_slot('sl')});

	# No primary to answer: the creation fails, saying why.
	$node->stop;
	my ($ret, $o, $e) = $standby->psql('postgres',
		q{SELECT pg_create_logical_replication_slot('sn', 'test_decoding')});
	like($e, qr/did not answer, in 2000 ms, the request that it keep the values the slot may decode/,
		'with no primary to answer, a slot\'s creation fails, saying why');
	$node->start;
	$node->wait_for_replay_catchup($standby);

	# Promoted while a slot's creation waits: it fails at once.
	$g = $node->background_psql('postgres');
	$g->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-gc-roots-scanned', 'wait')});
	$g->query_until(qr/collecting/, q{
		\echo collecting
		SELECT test_chunk_gc(true) IS NOT NULL;
		\echo collected
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0
		FROM pg_stat_activity WHERE wait_event = 'prolly-gc-roots-scanned'})
	  or die 'the collection did not stop';
	my $p = $standby->background_psql('postgres', on_error_stop => 0);
	$p->query_until(qr/creating/, q{
		\echo creating
		SELECT pg_create_logical_replication_slot('sp', 'test_decoding');
		\echo created
	});
	$standby->poll_query_until('postgres', q{SELECT count(*) > 0
		FROM pg_stat_activity WHERE wait_event = 'ReplicationSlotUpstreamHorizon'})
	  or die 'the creation did not wait';
	$standby->promote;
	$p->query_until(qr/created/, '');
	like($p->{stderr}, qr/Recovery ended while the slot was being created/,
		'promoted meanwhile, a slot\'s creation fails, saying why');
	$p->quit;
	sql(q{SELECT injection_points_wakeup('prolly-gc-roots-scanned')});
	$g->query_until(qr/collected/, '');
	$g->quit;
	$standby->stop;
}

# A slot file of the version before standby_restart_lsn (5) is read, with
# none: the field is its last 8 bytes, and the checksum covers the rest.
my @crc;
for my $i (0 .. 255)
{
	my $c = $i;
	$c = ($c & 1) ? (($c >> 1) ^ 0x82F63B78) : ($c >> 1) for 1 .. 8;
	$crc[$i] = $c;
}
sub crc32c
{
	my $c = 0xFFFFFFFF;
	$c = $crc[($c ^ $_) & 0xFF] ^ ($c >> 8) for unpack('C*', $_[0]);
	return $c ^ 0xFFFFFFFF;
}
$standby->stop;
my $restart = sql(q{SELECT restart_lsn FROM pg_replication_slots
	WHERE slot_name = 'sb'});
$node->stop;
my $state = $node->data_dir . '/pg_replslot/sb/state';
my $bytes = slurp_file($state);
my ($magic, $sum, $version, $len) = unpack('V4', $bytes);
is($version, 6, 'a slot file is version 6');
is($sum, crc32c(substr($bytes, 8, 8 + $len)), 'whose checksum this computes');
my $v5 = pack('V2', 5, $len - 8) . substr($bytes, 16, $len - 8);
open(my $fh, '>', $state) or die "$state: $!";
binmode $fh;
print $fh pack('V2', $magic, crc32c($v5)) . $v5;
close($fh);
$node->start;
is( sql(q{SELECT restart_lsn || ' ' || (standby_restart_lsn IS NULL)
		FROM pg_replication_slots WHERE slot_name = 'sb'}),
	"$restart true",
	'a version 5 slot file is read, its position kept, with no standby position');

$node->stop;
done_testing();
