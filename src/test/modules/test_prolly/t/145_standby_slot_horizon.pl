# Copyright (c) 2026, PostgreSQL Global Development Group

# A logical slot on a standby holds the primary's collections back: the
# standby's hot standby feedback carries its slots' oldest restart_lsn
# (sent with hot_standby_feedback off too, so no xmin holds the primary's
# dead rows here), which the primary keeps in the standby's physical slot
# (across a restart, with the standby away), and a collection keeps the
# values the WAL names from there.  A slot's creation sends the feedback at
# once and waits for the primary to acknowledge it, so a collection right
# after, with no feedback since (wal_receiver_status_interval as by
# default), keeps what the slot decodes; a collection running as the slot
# is created is waited for, and the slot starts after it; a cascaded
# standby's slot the same, the standby between passing the primary's
# acknowledgement on.  The standby's slot decodes a value whose row was
# deleted and pruned on the primary and collected; once the slot is
# dropped, the primary collects it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('horizon_primary');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', 'autovacuum = off');
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
my $horizon = 'SELECT test_logical_horizon() IS NOT NULL';

sql(q{SELECT pg_create_physical_replication_slot('sb')});
sql('CREATE EXTENSION test_prolly');
sql('CREATE SCHEMA d VERSIONED');
sql('CREATE TABLE d.docs (id int PRIMARY KEY, t text)');
# (a row that stays, so VACUUM prunes the page rather than cutting it off)
sql(q{INSERT INTO d.docs VALUES (0, 'stays')});

$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('horizon_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
# (wal_receiver_status_interval as by default: no feedback for 10 s)
$standby->append_conf('postgresql.conf', qq{
primary_slot_name = 'sb'
hot_standby_feedback = off
});
$standby->start;
$node->wait_for_replay_catchup($standby);

is(sql($horizon), 'f', 'with no logical slot anywhere, nothing is held');

# A value collected before the slot exists: the slot starts after its
# swap, and doesn't decode its row.
sql('INSERT INTO d.docs VALUES (2, ' . value('before') . ')');
my $before = root(2);
sql('DELETE FROM d.docs WHERE id = 2');
sql('VACUUM d.docs');
gc();
is(has($before), 'f', 'a value collected before any slot is freed');
my $collected = sql('SELECT pg_current_wal_lsn()');
$node->wait_for_replay_catchup($standby);

$standby->create_logical_slot_on_standby($node, 'ss', 'postgres');
is(sql($horizon), 't',
	'as the standby\'s slot is created, the primary keeps its position');
is( $standby->safe_psql(
		'postgres', qq{SELECT restart_lsn >= '$collected'
		FROM pg_replication_slots WHERE slot_name = 'ss'}),
	't',
	'and the slot starts after what was collected before it');

# At once, no feedback since: a value named after the slot's position, its
# row gone and pruned, and the primary's store collected.
sql('INSERT INTO d.docs VALUES (1, ' . value('decoded') . ')');
my $root = root(1);
sql('DELETE FROM d.docs WHERE id = 1');
sql('VACUUM d.docs');
gc();
is(has($root), 't', 'the primary keeps a value the standby\'s slot may decode');

$node->wait_for_replay_catchup($standby);
my $changes = $standby->safe_psql('postgres',
	q{SELECT string_agg(data, E'\n') FROM
	pg_logical_slot_get_changes('ss', NULL, NULL, 'include-xids', '0')});
my $v = sql('SELECT ' . value('decoded'));
like($changes, qr/table d\.docs: INSERT: id\[integer\]:1 t\[text\]:'$v'/,
	'the standby\'s slot decodes it, its row gone');
unlike($changes, qr/id\[integer\]:2/,
	'and not the row of the value collected before it');

# A collection running as a slot is created, which read the positions
# before: the creation waits for it, and the slot starts after its swap.
SKIP:
{
	skip 'Injection points not supported by this build', 4
	  unless $ENV{enable_injection_points} eq 'yes';
	sql('CREATE EXTENSION injection_points');
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

	my ($out, $err);
	my $create = IPC::Run::start(
		[
			'pg_recvlogical',
			'--dbname' => $standby->connstr('postgres'),
			'--plugin' => 'test_decoding',
			'--slot' => 'sc',
			'--create-slot'
		],
		'>' => \$out,
		'2>' => \$err);
	ok( $standby->poll_query_until(
			'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
			WHERE wait_event = 'ReplicationSlotUpstreamHorizon'}),
		'a slot\'s creation waits for a collection already running');
	my $waiting = sql('SELECT pg_current_wal_lsn()');

	sql(q{SELECT injection_points_wakeup('prolly-gc-roots-scanned')});
	$g->query_until(qr/collected/, '');
	$g->quit;
	ok( $standby->poll_query_until(
			'postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
			WHERE wait_event = 'ReplicationSlotUpstreamHorizon'}),
		'which goes on once the collection is done');
	sql('SELECT pg_log_standby_snapshot()');
	$create->finish();
	is($standby->slot('sc')->{'slot_type'}, 'logical', 'the slot is created');
	is( $standby->safe_psql(
			'postgres', qq{SELECT restart_lsn > '$waiting'
			FROM pg_replication_slots WHERE slot_name = 'sc'}),
		't',
		'and starts after the collection\'s swap');
	$standby->safe_psql('postgres',
		q{SELECT pg_drop_replication_slot('sc')});
}

# The physical slot keeps it while the standby is away, across a restart.
$standby->stop;
$node->restart;
is(sql($horizon), 't', 'the physical slot keeps the standby\'s position');

# Once the standby's slot is dropped, the primary collects the value.
$standby->start;
$standby->safe_psql('postgres', q{SELECT pg_drop_replication_slot('ss')});
ok($node->poll_query_until('postgres', $horizon, 'f'),
	'the dropped slot no longer holds the primary');
gc();
is(has($root), 'f', 'and the value is collected');

# A slot on a standby of the standby: its creation waits for the primary's
# acknowledgement, which the standby between passes on, and a collection
# at once keeps what it decodes.
$standby->backup('cascade');
my $cascade = PostgreSQL::Test::Cluster->new('horizon_cascade');
$cascade->init_from_backup($standby, 'cascade', has_streaming => 1);
$cascade->append_conf('postgresql.conf', qq{
primary_slot_name = ''
hot_standby_feedback = off
});
$cascade->start;
$standby->wait_for_replay_catchup($cascade, $node);
$cascade->create_logical_slot_on_standby($node, 'cs', 'postgres');
is(sql($horizon), 't',
	'as a cascaded standby\'s slot is created, the primary keeps its position');
sql('INSERT INTO d.docs VALUES (3, ' . value('cascaded') . ')');
my $cascaded = root(3);
sql('DELETE FROM d.docs WHERE id = 3');
sql('VACUUM d.docs');
gc();
is(has($cascaded), 't',
	'the primary keeps a value a cascaded standby\'s slot may decode');
$standby->wait_for_replay_catchup($cascade, $node);
$v = sql('SELECT ' . value('cascaded'));
like(
	$cascade->safe_psql(
		'postgres', q{SELECT string_agg(data, E'\n') FROM
		pg_logical_slot_get_changes('cs', NULL, NULL, 'include-xids', '0')}),
	qr/table d\.docs: INSERT: id\[integer\]:3 t\[text\]:'$v'/,
	'which decodes it, its row gone');
$cascade->safe_psql('postgres', q{SELECT pg_drop_replication_slot('cs')});
$cascade->stop;

$standby->stop;
$node->stop;
done_testing();
