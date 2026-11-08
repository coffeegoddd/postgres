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

sub oid_of
{
	my ($rel) = @_;
	return sql(qq{SELECT '$rel'::regclass::oid});
}

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

# Names each a prefix of another's, as an object's or a table's key is its kind and name: views
# alike but for their names, tables, and indexes of one table.  A view of the shorter name is not
# the longer one's, nor is either made again where the target keeps it.
sql(q{SET ROLE app_owner; SET search_path = app;
	CREATE TABLE t (id integer PRIMARY KEY, v integer);
	CREATE TABLE t2 (id integer PRIMARY KEY, v integer);
	CREATE INDEX t_v ON t (v);
	CREATE VIEW v AS SELECT id FROM t2;
	INSERT INTO t VALUES (1, 1); INSERT INTO t2 VALUES (1, 2)});
my $first = commit_root();
sql(q{SET ROLE app_owner; SET search_path = app;
	CREATE VIEW v2 AS SELECT id FROM t2;
	CREATE INDEX t_v2 ON t (v);
	INSERT INTO t VALUES (2, 2)});
my $second = commit_root();
sql(q{SET ROLE app_owner; SET search_path = app; DROP VIEW v; DROP TABLE t; INSERT INTO t2 VALUES (2, 3)});
my $third = commit_root();

sub names
{
	return sql(q{SELECT string_agg(c.relname, ',' ORDER BY c.relname) FROM pg_class c
		WHERE c.relnamespace = 'app'::regnamespace AND c.relkind IN ('r', 'i', 'v')});
}
move_to($first, 'names: to the first, from one with only the longer');
is(names(), 't,t2,t2_pkey,t_pkey,t_v,v', 'names: each of the first, and nothing longer');
my %oid = map { $_ => oid_of("app.$_") } qw(v t2);
move_to($second, 'names: to the second, which adds the longer');
is(names(), 't,t2,t2_pkey,t_pkey,t_v,t_v2,v,v2', 'names: the longer made beside the shorter');
is(oid_of('app.v'), $oid{v}, 'names: the shorter view kept, not taken for the longer');
$oid{v2} = oid_of('app.v2');
move_to($third, 'names: to the third, which drops the shorter');
is(names(), 't2,t2_pkey,v2', 'names: the shorter gone, the longer there');
is(oid_of('app.v2'), $oid{v2}, 'names: the longer view kept, not taken for the shorter');
is(oid_of('app.t2'), $oid{t2}, 'names: and the longer table');
move_to($second, 'names: and back');
is(oid_of('app.v2'), $oid{v2}, 'names: the longer view kept still');
sql("DROP SCHEMA app CASCADE; $schema");

# A partition with what it has of its own (an index, a constraint, a trigger, a policy, statistics)
# and what it has of its table's (an index, a constraint, a trigger), dropped, then detached.
my $partition = q{
	CREATE TABLE ev_a PARTITION OF ev FOR VALUES FROM (0) TO (10);
	CREATE INDEX ev_a_note ON ev_a (note);
	ALTER TABLE ev_a ADD CONSTRAINT ev_a_short CHECK (length(note) < 10);
	CREATE TRIGGER ev_a_trg AFTER INSERT ON ev_a FOR EACH ROW EXECUTE FUNCTION app.loud();
	CREATE POLICY ev_a_pol ON ev_a USING (id > 0);
	CREATE STATISTICS ev_a_st ON id, at FROM ev_a;
	INSERT INTO ev VALUES (1, 1, 'one')};
sql(q{SET ROLE app_owner; SET search_path = app;
	CREATE FUNCTION app.loud() RETURNS trigger LANGUAGE plpgsql AS
		$$BEGIN RAISE WARNING 'user code ran: % %', TG_TABLE_NAME, TG_OP; RETURN NULL; END$$;
	CREATE TABLE ev (id integer, at integer, note text, PRIMARY KEY (id, at), CONSTRAINT ev_pos CHECK (id > 0))
		PARTITION BY RANGE (at);
	CREATE INDEX ev_when ON ev (at);
	CREATE TRIGGER ev_trg AFTER INSERT ON ev FOR EACH ROW EXECUTE FUNCTION app.loud();
	CREATE TABLE ev_b PARTITION OF ev FOR VALUES FROM (10) TO (20);
	INSERT INTO ev VALUES (2, 11, 'two');} . $partition);

sub of_partition
{
	return sql(q{SELECT (SELECT string_agg(relname, ',' ORDER BY relname) FROM pg_class
			WHERE relnamespace = 'app'::regnamespace AND relname LIKE 'ev\_a%') || ' ' ||
		(SELECT string_agg(tgname, ',' ORDER BY tgname) FROM pg_trigger
			WHERE tgrelid = to_regclass('app.ev_a') AND NOT tgisinternal) || ' ' ||
		(SELECT string_agg(conname, ',' ORDER BY conname) FROM pg_constraint
			WHERE conrelid = to_regclass('app.ev_a') AND contype = 'c') || ' ' ||
		(SELECT string_agg(polname, ',') FROM pg_policy WHERE polrelid = to_regclass('app.ev_a')) || ' ' ||
		(SELECT string_agg(stxname, ',') FROM pg_statistic_ext WHERE stxrelid = to_regclass('app.ev_a'))});
}
my $whole = commit_root();
my $whole_has = of_partition();
like($whole_has, qr/ev_a_at_idx.*ev_a_note\b/, "partitions: it has what it has: $whole_has");
sql(q{SET ROLE app_owner; DROP TABLE app.ev_a});
my $dropped = commit_root();
sql(q{SET ROLE app_owner; SET search_path = app;} . $partition . q{; ALTER TABLE ev DETACH PARTITION ev_a});
my $detached = commit_root();
my $detached_has = of_partition();
my $log_at = -s $node->logfile;
move_to($whole, 'partitions: to the whole table');
%oid = map { $_ => oid_of("app.$_") } qw(ev ev_b);
move_to($dropped, 'partitions: to one dropped');
is(sql(q{SELECT count(*) FROM pg_class WHERE relnamespace = 'app'::regnamespace AND relname LIKE 'ev\_a%'}), '0',
	'partitions: it is gone, with its indexes');
is(oid_of('app.ev_b'), $oid{ev_b}, 'partitions: another is kept');
move_to($whole, 'partitions: back to the whole table');
is(of_partition(), $whole_has, 'partitions: it is back, with all it had');
is(oid_of('app.ev'), $oid{ev}, 'partitions: its table kept');
move_to($detached, 'partitions: to one detached');
is(sql(q{SELECT relispartition FROM pg_class WHERE oid = 'app.ev_a'::regclass}), 'f',
	'partitions: it is a table of its own');
is(of_partition(), $detached_has, 'partitions: with what it had of its own, and kept of its table');
move_to($whole, 'partitions: and back again');
is(of_partition(), $whole_has, 'partitions: with all it had as a partition');
unlike(slurp_file($node->logfile, $log_at), qr/user code ran/, 'partitions: and no trigger ran');
sql("DROP SCHEMA app CASCADE; $schema");

# Storage parameters alone changed (a fillfactor, a TOAST table's): the table is altered, and kept,
# its rows not written.
sql(q{SET ROLE app_owner; SET search_path = app; CREATE TABLE ff (id integer PRIMARY KEY, v text);
	INSERT INTO ff VALUES (1, 'x')});
my $plain = commit_root();
sql(q{SET ROLE app_owner; ALTER TABLE app.ff SET (fillfactor = 60, toast.autovacuum_enabled = false)});
my $filled = commit_root();
sql(q{SET ROLE app_owner; ALTER TABLE app.ff SET (fillfactor = 70)});
my $refilled = commit_root();
my $ff = sql(q{SELECT oid || ' ' || relfilenode FROM pg_class WHERE oid = 'app.ff'::regclass});
for my $to ([ $plain, 'none' ], [ $filled, '60 and TOAST' ], [ $refilled, '70, TOAST kept' ], [ $filled, '60 again' ],
	[ $plain, 'none again' ])
{
	my $stats = move_to($to->[0], "storage: to $to->[1]");
	like($stats, qr/ deleted 0 inserted 0 .* made 0 dropped 0 altered 1 /, "storage: to $to->[1], altered: $stats");
	is(sql(q{SELECT oid || ' ' || relfilenode FROM pg_class WHERE oid = 'app.ff'::regclass}), $ff,
		"storage: to $to->[1], the table and its file kept");
}

done_testing();
