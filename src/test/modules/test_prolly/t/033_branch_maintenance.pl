# Copyright (c) 2026, PostgreSQL Global Development Group

# Maintenance reaches every branch: the database's oldest XID counts each
# branch's tables, autovacuum vacuums them, logical decoding decodes the
# session branch's changes and skips the others', and copying a database
# copies every branch.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_maintenance');
$node->init;
$node->append_conf('postgresql.conf', qq{
wal_level = logical
autovacuum = off
autovacuum_naptime = 1s
autovacuum_vacuum_threshold = 10
autovacuum_vacuum_scale_factor = 0
log_autovacuum_min_duration = 0
});
$node->start;

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }

sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 1000) g;
	SELECT dolt_branch('feature');
});
my $feature_xid = sql(q{SELECT relfrozenxid FROM pg_class WHERE oid = 'app.t'::regclass},
	'postgres/feature');

# Freezing main's tables leaves the branch's copy as old as it was; the
# database's oldest XID must not pass it.
sql(q{SELECT txid_current()}) for 1 .. 20;
sql(q{VACUUM (FREEZE)});
cmp_ok(sql(q{SELECT age(relfrozenxid) FROM pg_class WHERE oid = 'app.t'::regclass}), '<',
	sql(qq{SELECT age('$feature_xid'::xid)}), 'main\'s copy is frozen past the branch\'s');
cmp_ok(sql(q{SELECT age(datfrozenxid) FROM pg_database WHERE datname = 'postgres'}), '>=',
	sql(qq{SELECT age('$feature_xid'::xid)}),
	'the database\'s oldest XID counts the branch\'s copy');

# Each branch's copy has its own activity statistics.
sql(q{DELETE FROM app.t WHERE id <= 500}, 'postgres/feature');
my $dead = q{SELECT n_dead_tup FROM pg_stat_user_tables WHERE relid = 'app.t'::regclass};
ok($node->poll_query_until('postgres/feature', $dead, '500'),
	'the branch\'s copy counts its dead rows');
is(sql($dead), '0', 'main\'s copy does not');

# Autovacuum vacuums the branch's copy.
$node->append_conf('postgresql.conf', 'autovacuum = on');
$node->reload;
ok( $node->poll_query_until('postgres/feature',
		qq{SELECT relfrozenxid <> '$feature_xid' FROM pg_class WHERE oid = 'app.t'::regclass}),
	'autovacuum vacuums the branch\'s copy');
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->reload;

# Autovacuum holds the branch it is vacuuming: the branch can't be deleted
# under it.  Its vacuum of the copy is slowed down to be caught at it.
sql(q{SELECT dolt_branch('slow')});
sql(q{
	CREATE TABLE app.slow (id int, v text)
	  WITH (autovacuum_vacuum_cost_delay = 100, autovacuum_vacuum_cost_limit = 1);
	INSERT INTO app.slow SELECT g, repeat('x', 100) FROM generate_series(1, 20000) g;
	DELETE FROM app.slow;
}, 'postgres/slow');
$node->append_conf('postgresql.conf', 'autovacuum = on');
$node->reload;
ok( $node->poll_query_until('postgres',
		q{SELECT count(*) > 0 FROM pg_stat_activity WHERE query LIKE 'autovacuum: VACUUM%app.slow%'}),
	'autovacuum is vacuuming the branch\'s table');
my ($ret, $stdout, $stderr) = $node->psql('postgres', q{SELECT dolt_branch('-d', 'slow')});
like($stderr, qr/branch "slow" is checked out in another session/,
	'the branch can\'t be deleted meanwhile');
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->reload;
sql(q{SELECT pg_terminate_backend(pid) FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker'});
ok( $node->poll_query_until('postgres',
		q{SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'autovacuum worker'}),
	'the worker is gone');
sql(q{SELECT dolt_branch('-d', 'slow')});
is(sql(q{SELECT count(*) FROM dolt_branches() WHERE name = 'slow'}), '0',
	'then it can be');

# A relation made in a transaction that then checks out a copy of it: the
# copy gets the rows inserted on it, and counts them apart from the
# original's.
sql(q{
	BEGIN;
	CREATE TABLE app.stats_test (id int);
	INSERT INTO app.stats_test VALUES (1);
	SELECT dolt_branch('stats_copy');
	SELECT dolt_checkout('stats_copy');
	INSERT INTO app.stats_test SELECT generate_series(1, 10);
	COMMIT;
});
my $ins = q{SELECT n_tup_ins FROM pg_stat_user_tables WHERE relid = 'app.stats_test'::regclass};
is(sql(q{SELECT count(*) FROM app.stats_test}, 'postgres/stats_copy'), '11',
	'the rows inserted on the copy are in the copy');
is(sql(q{SELECT count(*) FROM app.stats_test}), '1', 'not in the original');
ok($node->poll_query_until('postgres/stats_copy', $ins, '10'),
	'the copy counts the rows inserted on it');
is(sql($ins), '1', 'the original, those inserted on main');

# Logical decoding on main decodes main's changes and skips the branch's.
sql(q{SELECT pg_create_logical_replication_slot('s', 'test_decoding')});
sql(q{INSERT INTO app.t VALUES (2001, 'main')});
sql(q{INSERT INTO app.t VALUES (3001, 'feature')}, 'postgres/feature');
sql(q{INSERT INTO app.t VALUES (2002, 'main')});
my $changes = sql(q{SELECT string_agg(data, E'\n') FROM pg_logical_slot_get_changes('s', NULL, NULL)
	WHERE data LIKE 'table app.t:%'});
like($changes, qr/2001.*\n.*2002/, 'main\'s changes are decoded');
unlike($changes, qr/3001/, 'the branch\'s are not');

# A truncation too: of the branch's copy, and of a table only it has.
sql(q{
	CREATE TABLE app.tr (id int PRIMARY KEY);
	INSERT INTO app.tr VALUES (1);
	SELECT dolt_branch('truncating');
});
sql(q{SELECT pg_logical_slot_get_changes('s', NULL, NULL)});
sql(q{
	CREATE TABLE app.only_here (id int PRIMARY KEY);
	TRUNCATE app.tr;
	TRUNCATE app.only_here;
}, 'postgres/truncating');
sql(q{TRUNCATE app.tr});
$changes = sql(q{SELECT string_agg(data, E'\n') FROM pg_logical_slot_get_changes('s', NULL, NULL)
	WHERE data LIKE '%TRUNCATE%'});
is($changes, 'table app.tr: TRUNCATE: (no-flags)', 'only main\'s truncation is decoded');
sql(q{SELECT pg_drop_replication_slot('s')});

# Copying the database copies every branch's relations.
my $rows = q{SELECT count(*) || ':' || sum(id) FROM app.t};
my $main_rows = sql($rows);
my $feature_rows = sql($rows, 'postgres/feature');
sql(q{CREATE DATABASE copied TEMPLATE postgres STRATEGY wal_log});
is(sql($rows, 'copied'), $main_rows, 'the copied database has main');
is(sql($rows, 'copied/feature'), $feature_rows, 'and the branch');

$node->stop;

# With wal_level minimal, a relation made in a transaction skips WAL and is
# synced at commit; a copy of it would be neither, so making a branch from
# it is refused.  Changing branches copies nothing, and is allowed.
my $minimal = PostgreSQL::Test::Cluster->new('branch_minimal');
$minimal->init;
$minimal->append_conf('postgresql.conf', qq{
wal_level = minimal
max_wal_senders = 0
});
$minimal->start;
my ($mret, $mout, $merr) = $minimal->psql('postgres', q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY);
	INSERT INTO app.t VALUES (1);
	SELECT dolt_commit('-Am', 'first');
	SELECT dolt_branch('other');
	SELECT dolt_checkout('other');	-- (its copies made)
	SELECT dolt_checkout('main');
	BEGIN;
	CREATE TABLE app.new_here (id int);
	SELECT dolt_checkout('-b', 'from_new');
});
like($merr,
	qr/cannot copy relation "new_here" to a branch in the transaction that made it/,
	'a relation that skips WAL isn\'t copied to a new branch');

# (other's copies exist already: the checkouts copy nothing)
$minimal->safe_psql('postgres', q{
	BEGIN;
	CREATE TABLE app.made_here (id int);
	INSERT INTO app.made_here VALUES (7);
	SELECT dolt_checkout('other');
	SELECT dolt_checkout('main');
	COMMIT;
});

# A branch first used here, whose copies are made from its head rather than
# shared with another branch's (a clone's): its new relations skip WAL too.
my $remote = $minimal->basedir . '/remote';
mkdir $remote or die "mkdir $remote: $!";
$minimal->safe_psql('postgres', qq{
	SELECT dolt_remote('add', 'origin', 'file://$remote');
	SELECT dolt_push('origin', 'other');
});
$minimal->safe_psql('postgres', 'CREATE DATABASE cloned');
$minimal->safe_psql('cloned', qq{SELECT dolt_clone('file://$remote')});
is($minimal->safe_psql('cloned', q{
	SELECT dolt_checkout('other');
	SELECT active_branch(), count(*) FROM app.t;
}), "{0,\"Switched to branch 'other'\"}\nother|1",
	'a cloned branch is checked out under wal_level minimal');

# Their storage was synced at commit: a crash loses none of it.
$minimal->stop('immediate');
$minimal->start;
is($minimal->safe_psql('postgres', 'SELECT id FROM app.made_here'), '7',
	'a relation made before a branch change survives a crash');
is($minimal->safe_psql('cloned/other', 'SELECT count(*) FROM app.t'), '1',
	'a cloned branch\'s copies survive a crash');
$minimal->stop;
done_testing();
