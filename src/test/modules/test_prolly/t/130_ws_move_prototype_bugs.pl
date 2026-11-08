# Copyright (c) 2026, PostgreSQL Global Development Group

# Moves in place a first build got wrong, each kept: objects and tables whose names are each a
# prefix of another's; a partition, with an index and a constraint of its own, that the target
# drops or detaches; and a table whose fillfactor alone changes, which is altered, not made again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move_bugs');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";

sub sql
{
	my ($sql) = @_;
	return $node->safe_psql('postgres', "$on$sql");
}

my $commits = 0;
sub commit_root
{
	$commits++;
	return sql(qq{SELECT test_capture_commit_on('app', 'main', 'c$commits')});
}

sub oid_of { my ($rel) = @_; return sql(qq{SELECT '$rel'::regclass::oid}); }

sub move_to
{
	my ($root, $name) = @_;
	sql(qq{SELECT test_ws_move_prepare('app', '$root')});
	my $stats = sql(qq{SELECT test_ws_move('app', '$root')});
	is(sql(q{SELECT test_ws_root_capture('app')}), $root, "$name: the tables hold the root");
	is(sql(q{SELECT test_capture_barrier('app')}), $root, "$name: and capture has it");
	return $stats;
}

# (a role whose objects can travel in a commit, and that a commit marks)
$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
my $schema = 'CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner';
sql($schema);

# Names each a prefix of another's: tables, indexes, views, functions.
sql(q{SET ROLE app_owner; SET search_path = app;
	CREATE TABLE t (id integer PRIMARY KEY, v integer);
	CREATE TABLE t2 (id integer PRIMARY KEY, v integer);
	CREATE INDEX t_v ON t (v);
	CREATE INDEX t_v2 ON t2 (v);
	CREATE VIEW v AS SELECT id FROM t;
	CREATE FUNCTION f(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1';
	INSERT INTO t VALUES (1, 1); INSERT INTO t2 VALUES (1, 2)});
my $a = commit_root();
sql(q{SET ROLE app_owner; SET search_path = app;
	CREATE OR REPLACE VIEW v AS SELECT id, v FROM t;
	CREATE VIEW v2 AS SELECT id FROM t2;
	CREATE FUNCTION f2(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1 + 1';
	DROP INDEX t_v; CREATE INDEX t_v ON t (v, id);
	INSERT INTO t VALUES (2, 2)});
my $b = commit_root();
sql(q{SET ROLE app_owner; SET search_path = app; DROP VIEW v; DROP FUNCTION f(integer); DROP TABLE t CASCADE; INSERT INTO t2 VALUES (2, 3)});
my $c = commit_root();
move_to($a, 'names: to the first, from what dropped the shorter');
is(sql(q{SELECT string_agg(c.relname, ',' ORDER BY c.relname) FROM pg_class c
	WHERE c.relnamespace = 'app'::regnamespace AND c.relkind IN ('r', 'i', 'v')}),
	't,t2,t2_pkey,t_pkey,t_v,t_v2,v', 'names: each of the first, and nothing longer');
is(sql(q{SELECT string_agg(proname, ',' ORDER BY proname) FROM pg_proc WHERE pronamespace = 'app'::regnamespace}),
	'f', 'names: the function the first has, not the longer one');
move_to($b, 'names: to the second, which adds the longer ones');
is(sql(q{SELECT pg_get_indexdef('app.t_v'::regclass)}), 'CREATE INDEX t_v ON app.t USING btree (v, id)',
	'names: the index the second made again, not its longer neighbour');
move_to($c, 'names: to the third, which drops the shorter');
move_to($b, 'names: and back');
sql("DROP SCHEMA app CASCADE; $schema");

# A partition with an index and a constraint of its own, dropped, then detached.
sql(q{SET ROLE app_owner; SET search_path = app;
	CREATE TABLE ev (id integer, at integer, note text, PRIMARY KEY (id, at)) PARTITION BY RANGE (at);
	CREATE TABLE ev_a PARTITION OF ev FOR VALUES FROM (0) TO (10);
	CREATE TABLE ev_b PARTITION OF ev FOR VALUES FROM (10) TO (20);
	CREATE INDEX ev_a_note ON ev_a (note);
	ALTER TABLE ev_a ADD CONSTRAINT ev_a_short CHECK (length(note) < 10);
	INSERT INTO ev VALUES (1, 1, 'one'), (2, 11, 'two')});
my $whole = commit_root();
sql(q{SET ROLE app_owner; DROP TABLE app.ev_a});
my $dropped = commit_root();
sql(q{SET ROLE app_owner; SET search_path = app;
	CREATE TABLE ev_a PARTITION OF ev FOR VALUES FROM (0) TO (10);
	CREATE INDEX ev_a_note ON ev_a (note);
	ALTER TABLE ev_a ADD CONSTRAINT ev_a_short CHECK (length(note) < 10);
	INSERT INTO ev VALUES (1, 1, 'one');
	ALTER TABLE ev DETACH PARTITION ev_a});
my $detached = commit_root();
move_to($whole, 'partitions: to the whole table');
move_to($dropped, 'partitions: to one dropped');
is(sql(q{SELECT count(*) FROM pg_class WHERE relnamespace = 'app'::regnamespace AND relname LIKE 'ev\_a%'}), '0',
	'partitions: it is gone, with its index and constraint');
move_to($whole, 'partitions: back to the whole table');
is(sql(q{SELECT (SELECT count(*) FROM pg_indexes WHERE indexname = 'ev_a_note') ||
	(SELECT count(*) FROM pg_constraint WHERE conname = 'ev_a_short')::text}), '11',
	'partitions: it is back, with its index and constraint');
move_to($detached, 'partitions: to one detached');
is(sql(q{SELECT relispartition FROM pg_class WHERE oid = 'app.ev_a'::regclass}), 'f',
	'partitions: it is a table of its own');
move_to($whole, 'partitions: and back again');
is(sql(q{SELECT count(*) FROM app.ev}), '2', 'partitions: with its rows');
sql("DROP SCHEMA app CASCADE; $schema");

# A fillfactor alone changed: the table is altered, and kept.
sql(q{SET ROLE app_owner; SET search_path = app; CREATE TABLE ff (id integer PRIMARY KEY, v text); INSERT INTO ff VALUES (1, 'x')});
my $plain = commit_root();
sql(q{SET ROLE app_owner; ALTER TABLE app.ff SET (fillfactor = 60)});
my $filled = commit_root();
my $ff = oid_of('app.ff');
my $stats = move_to($plain, 'fillfactor: to none');
like($stats, qr/ made 0 dropped 0 altered 1 /, "fillfactor: altered, not made again: $stats");
is(oid_of('app.ff'), $ff, 'fillfactor: the table is kept');
is(sql(q{SELECT reloptions IS NULL FROM pg_class WHERE oid = 'app.ff'::regclass}), 't', 'fillfactor: and has none');
$stats = move_to($filled, 'fillfactor: to 60');
like($stats, qr/ made 0 dropped 0 altered 1 /, "fillfactor: altered again: $stats");
is(oid_of('app.ff'), $ff, 'fillfactor: still the table it was');

done_testing();
