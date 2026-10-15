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

# Logical decoding on main decodes main's changes and skips the branch's.
sql(q{SELECT pg_create_logical_replication_slot('s', 'test_decoding')});
sql(q{INSERT INTO app.t VALUES (2001, 'main')});
sql(q{INSERT INTO app.t VALUES (3001, 'feature')}, 'postgres/feature');
sql(q{INSERT INTO app.t VALUES (2002, 'main')});
my $changes = sql(q{SELECT string_agg(data, E'\n') FROM pg_logical_slot_get_changes('s', NULL, NULL)
	WHERE data LIKE 'table app.t:%'});
like($changes, qr/2001.*\n.*2002/, 'main\'s changes are decoded');
unlike($changes, qr/3001/, 'the branch\'s are not');
sql(q{SELECT pg_drop_replication_slot('s')});

# Copying the database copies every branch's relations.
my $rows = q{SELECT count(*) || ':' || sum(id) FROM app.t};
my $main_rows = sql($rows);
my $feature_rows = sql($rows, 'postgres/feature');
sql(q{CREATE DATABASE copied TEMPLATE postgres STRATEGY wal_log});
is(sql($rows, 'copied'), $main_rows, 'the copied database has main');
is(sql($rows, 'copied/feature'), $feature_rows, 'and the branch');

$node->stop;
done_testing();
