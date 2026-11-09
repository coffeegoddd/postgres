# Copyright (c) 2026, PostgreSQL Global Development Group

# Capture keeps each versioned schema's definitions as of what it has applied: a transaction that
# changed a branch's catalogs tells it, as it commits, of each of the branch's schemas whose
# definitions changed, and of each gone; a new slot reads them all as it begins.  Whatever changed
# them (a column, a view, a function, a comment, a grant, a rename, a table made or dropped, a schema
# made or dropped, a branch given a working set, at its head or an older commit, or given it back),
# they're what a session reads.  Definitions a commit can't hold are kept as such, with why, and the
# DDL that made them so isn't refused.  What a transaction holds for capture is let go of once applied,
# and what's kept outlasts a restart and a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('capture_definitions');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on { return "SET catalog_branch = $_[0]; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n"; }
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', on($branch) . $sql);
}
# (capture caught up with all committed on the branch)
sub caught_up { sql($_[0], "SELECT test_branch_barrier($_[0]) IS NOT NULL"); }
# (capture's definitions of a schema of a branch, as of now: what a session reads, or NULL)
sub kept { caught_up($_[0]); return sql($_[0], qq{SELECT coalesce(test_capture_definitions('$_[1]'), 'none')}); }
sub now_defs { return sql($_[0], qq{SELECT test_ws_definitions('$_[1]')}); }
# (the same, by the slot's position, as a barrier would build a root a commit can't hold)
sub kept_by_wait
{
	$node->safe_psql('postgres', q{SELECT test_capture_wait(pg_current_wal_insert_lsn())});
	return sql($_[0], qq{SELECT coalesce(test_capture_definitions('$_[1]'), 'none')});
}
sub same
{
	my ($branch, $schema, $name) = @_;
	is(kept($branch, $schema), now_defs($branch, $schema), $name);
}
my $told = q{SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'capture/told_defs/%'};

# (what a commit holds is owned by roles that travel)
$node->safe_psql('postgres', q{CREATE ROLE app_owner; CREATE ROLE reader;
	SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner', 'reader']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, v text); INSERT INTO app.t VALUES (1, 'a')});
sql($main, "SELECT test_branch_link('main', $main)");
same($main, 'app', 'as capture begins, a schema\'s definitions are what a session reads');

my $before = now_defs($main, 'app');
sql($main, q{SET ROLE app_owner; ALTER TABLE app.t ADD COLUMN w integer});
isnt(now_defs($main, 'app'), $before, 'a column added changes them');
same($main, 'app', 'a column added');
sql($main, q{SET ROLE app_owner; CREATE VIEW app.v AS SELECT id, v FROM app.t});
same($main, 'app', 'a view made');
sql($main, q{SET ROLE app_owner; CREATE FUNCTION app.f(i integer) RETURNS integer LANGUAGE sql AS 'SELECT i + 1'});
same($main, 'app', 'a function made');
$before = now_defs($main, 'app');
sql($main, q{SET ROLE app_owner; COMMENT ON TABLE app.t IS 'the table'});
isnt(now_defs($main, 'app'), $before, 'a comment changes them');
same($main, 'app', 'a comment, which invalidates no cache');
sql($main, q{SET ROLE app_owner; GRANT SELECT ON app.t TO reader});
same($main, 'app', 'a grant');
sql($main, q{SET ROLE app_owner; CREATE TABLE app.u (k integer PRIMARY KEY); ALTER TABLE app.u RENAME TO u2});
same($main, 'app', 'a table made and renamed');
is(sql($main, q{SELECT keys = ARRAY['t', 'u2'] AND tables = ARRAY['app.t'::regclass::oid, 'app.u2'::regclass::oid]
	FROM pg_history.capture_definitions WHERE name = 'app'}), 't', 'its tables kept by oid, as the definitions name them');
sql($main, q{SET ROLE app_owner; DROP TABLE app.u2});
same($main, 'app', 'a table dropped');
is($node->safe_psql('postgres', $told), '0', 'what each transaction held for capture let go of');

# A schema made and dropped.
sql($main, q{CREATE SCHEMA app2 VERSIONED; GRANT ALL ON SCHEMA app2 TO app_owner; SET ROLE app_owner;
	CREATE TABLE app2.x (a integer PRIMARY KEY)});
same($main, 'app2', 'a second schema made');
my $app2 = sql($main, q{SELECT 'app2'::regnamespace::oid});
sql($main, q{DROP SCHEMA app2 CASCADE});
caught_up($main);
is($node->safe_psql('postgres', qq{SELECT count(*) FROM pg_history.capture_definitions WHERE nspid = $app2}), '0',
	'a schema dropped: its definitions gone');
is($node->safe_psql('postgres', qq{SELECT count(*) FROM pg_history.pointers WHERE name = 'capture/defs/$app2'}), '0',
	'and what held them');

# Definitions a commit can't hold: kept as such, and the DDL not refused.
sql($main, q{ALTER DEFAULT PRIVILEGES IN SCHEMA app GRANT SELECT ON TABLES TO reader});
is(kept_by_wait($main, 'app'), 'none', 'definitions a commit can\'t hold aren\'t kept');
like(sql($main, q{SELECT why FROM pg_history.capture_definitions WHERE name = 'app'}), qr/default privileges/,
	'with why');
sql($main, q{ALTER DEFAULT PRIVILEGES IN SCHEMA app REVOKE SELECT ON TABLES FROM reader});
same($main, 'app', 'and once they can be again, they are');
is(sql($main, q{SELECT why IS NULL FROM pg_history.capture_definitions WHERE name = 'app'}), 't', 'with no why');

# Kept through a restart, and a crash with what's told not yet applied.
$node->restart;
same($main, 'app', 'kept through a restart');
sql($main, q{SET ROLE app_owner; ALTER TABLE app.t ADD COLUMN z integer});
$node->stop('immediate');
$node->start;
same($main, 'app', 'told before a crash and applied after it');
is($node->safe_psql('postgres', $told), '0', 'and what held them for capture let go of');

# A new slot reads them as it begins.
# (turned off, capture drops its slot)
$node->safe_psql('postgres', q{ALTER SYSTEM SET capture_in_background = off; SELECT pg_reload_conf()});
$node->poll_query_until('postgres', q{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name LIKE 'prolly_capture_%'})
  or die 'capture\'s slot was never dropped';
sql($main, q{SET ROLE app_owner; ALTER TABLE app.t ADD COLUMN y integer});
$node->safe_psql('postgres', q{ALTER SYSTEM RESET capture_in_background; SELECT pg_reload_conf()});
same($main, 'app', 'a new slot reads them as it begins');

# A branch given a working set, at its head and at an older commit: its own schemas' definitions.
sql($main, q{SELECT test_vc_ops_commit('main', 'c1')});
my $c1 = $node->safe_psql('postgres', q{SELECT test_branch_head('main')});
sql($main, q{SET ROLE app_owner; ALTER TABLE app.t ADD COLUMN later integer});
sql($main, q{SELECT test_vc_ops_commit('main', 'c2')});
my $f = $node->safe_psql('postgres', q{SELECT test_branch_create('f', 'main')});
same($f, 'app', 'a branch given a working set at its head');
my $old = $node->safe_psql('postgres', qq{SELECT test_branch_create('old', 'main', '$c1')});
same($old, 'app', 'one at an older commit');
isnt(now_defs($old, 'app'), now_defs($f, 'app'), 'which differ from its head\'s');
my $f_app = sql($f, q{SELECT 'app'::regnamespace::oid});
$node->safe_psql('postgres', q{SELECT test_branch_dematerialize('f')});
caught_up($main);
is($node->safe_psql('postgres', qq{SELECT count(*) FROM pg_history.capture_definitions WHERE nspid = $f_app}), '0',
	'a branch giving its working set back: its definitions gone');
is($node->safe_psql('postgres', $told), '0', 'and nothing held for capture left');

done_testing();
