# Copyright (c) 2026, PostgreSQL Global Development Group

# A logical slot on a standby holds the primary's collections back: the
# standby's hot standby feedback carries its slots' oldest restart_lsn
# (sent with hot_standby_feedback off too, so no xmin holds the primary's
# dead rows here), which the primary keeps in the standby's physical slot
# (across a restart, with the standby away), and a collection keeps the
# values the WAL names from there.  The standby's slot decodes a value
# whose row was deleted and pruned on the primary and collected; once the
# slot is dropped, the primary collects it.

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
$standby->append_conf('postgresql.conf', qq{
primary_slot_name = 'sb'
hot_standby_feedback = off
wal_receiver_status_interval = 1
});
$standby->start;
$node->wait_for_replay_catchup($standby);

is(sql($horizon), 'f', 'with no logical slot anywhere, nothing is held');
$standby->create_logical_slot_on_standby($node, 'ss', 'postgres');
ok($node->poll_query_until('postgres', $horizon),
	'the standby\'s slot reaches the primary');

# A value named after the standby slot's position, its row gone and
# pruned, and the primary's store collected.
sql('INSERT INTO d.docs VALUES (1, ' . value('decoded') . ')');
my $root = sql('SELECT (test_toast_pointer(t)).root FROM d.docs WHERE id = 1');
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

$standby->stop;
$node->stop;
done_testing();
