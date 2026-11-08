# Copyright (c) 2026, PostgreSQL Global Development Group

# A versioned schema moved in place to and from roots a merge left holding constraint violations:
# a table with constraints set aside, here or in the target, is made again, with them set aside as
# the target has them; the rest are kept.  After each move the tables hold the target, and capture
# has it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move_viol');
$node->init(allows_streaming => 'logical');
# (the roots moved to are merged or read, and no commit holds them: no freer takes their chunks)
$node->append_conf('postgresql.conf',
	"autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\nchunk_free_in_background = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";

sub sql
{
	my ($sql) = @_;
	return $node->safe_psql('postgres', "$on$sql");
}

sub read_root { return sql(q{SELECT test_ws_root_capture('app')}); }
sub capture_root { return sql(q{SELECT test_capture_barrier('app')}); }
sub oid_of { my ($rel) = @_; return sql(qq{SELECT '$rel'::regclass::oid}); }

sub deferred
{
	return sql(q{SELECT c.relname || '|' || d.defkind::text || '|' || d.defname FROM pg_ws_deferred d
		JOIN pg_class c ON c.oid = d.defrelid WHERE c.relnamespace = 'app'::regnamespace ORDER BY 1});
}

sub move_to
{
	my ($root, $name) = @_;
	sql(qq{SELECT test_ws_move_prepare('app', '$root')});
	my $stats = sql(qq{SELECT test_ws_move('app', '$root')});
	note("$name: $stats");
	is(read_root(), $root, "$name: the tables hold the root");
	is(capture_root(), $root, "$name: and capture has it");
	return $stats;
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});

my $base = q{CREATE TABLE p (id integer PRIMARY KEY, code integer NOT NULL, CONSTRAINT code_one UNIQUE (code));
	CREATE TABLE c (id integer PRIMARY KEY, code integer REFERENCES p (code));
	CREATE TABLE t (id integer PRIMARY KEY, v integer);
	CREATE TABLE other (id integer PRIMARY KEY, v text);
	CREATE TABLE k (id integer PRIMARY KEY, pid integer REFERENCES p (id));
	CREATE VIEW pv AS SELECT id, code FROM p;
	CREATE MATERIALIZED VIEW pm AS SELECT count(*) AS n FROM p;
	INSERT INTO p VALUES (1, 7); INSERT INTO c VALUES (1, 7); INSERT INTO k VALUES (1, 1);
	INSERT INTO t VALUES (1, 1); INSERT INTO other VALUES (1, 'x');
	REFRESH MATERIALIZED VIEW pm};

# The root of a schema made of the base and then text, the schema dropped after.
my $made = 0;
sub root_of
{
	my ($text) = @_;
	$made++;
	my $root = sql(qq{SET client_min_messages = warning;
		CREATE SCHEMA s$made VERSIONED; GRANT ALL ON SCHEMA s$made TO app_owner;
		SET ROLE app_owner; SET search_path = s$made; $base; $text; RESET ROLE; SET search_path = public;
		SELECT test_ws_root_capture('s$made')});
	sql(qq{DROP SCHEMA s$made CASCADE});
	return $root;
}

sub merged
{
	my ($a, $ours, $theirs) = @_;
	return sql(qq{SELECT split_part(test_ws_merge_checked('$a', '} . root_of($ours) . q{', '} .
		root_of($theirs) . q{'), E'\n', 1)});
}

sql(qq{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner;
	SET search_path = app; $base});
my $a = read_root();
is(capture_root(), $a, 'capture starts where the tables are');

# Rows alike on a unique key, and another table's row changed: the definitions are the same.
my $m1 = merged($a, q{INSERT INTO p VALUES (2, 9); UPDATE other SET v = 'y'}, q{INSERT INTO p VALUES (3, 9)});
is(sql(qq{SELECT test_ws_violations('$m1')}), "unique p code_one 2\nunique p code_one 3",
	'the merge holds the rows a unique key refuses');

# A barrier over a schema with constraints set aside checks them by restoring its root, undone.
sql(qq{CREATE SCHEMA r VERSIONED; GRANT ALL ON SCHEMA r TO app_owner; SELECT test_ws_root_restore('$m1', 'r')});
is(sql(q{SELECT test_capture_barrier('r')}), $m1, 'a barrier over a schema with constraints set aside has its root');
sql(q{DROP SCHEMA r CASCADE});

my %oid = map { $_ => oid_of("app.$_") } qw(p c t other k);
my $stats = move_to($m1, 'to a root holding violations');
like($stats, qr/ made 2 dropped 0 altered 0 /, "the two tables with what is set aside are made: $stats");
is(deferred(), "c|f|c_code_fkey\np|u|code_one", 'the unique constraint is set aside, and the key that needs it');
is(sql(q{SELECT count(*) FROM pg_constraint WHERE conrelid IN ('app.p'::regclass, 'app.c'::regclass) AND contype IN ('u', 'f')}),
	'0', 'and neither is on its table');
is(sql(q{SELECT string_agg(id || ':' || code, ',' ORDER BY id) FROM app.p}), '1:7,2:9,3:9', 'the rows alike are both there');
is(sql(q{SELECT v FROM app.other}), 'y', 'a table kept has its row');
isnt(oid_of('app.p'), $oid{p}, 'the table with what is set aside is made again');
isnt(oid_of('app.c'), $oid{c}, 'and the one with the key that needs it');
is(oid_of('app.t'), $oid{t}, 'a table with nothing set aside is kept');
is(oid_of('app.other'), $oid{other}, 'and one whose rows differ');
is(oid_of('app.k'), $oid{k}, 'and one with a key to a table made again');
is(sql(q{SELECT conname FROM pg_constraint WHERE conrelid = 'app.k'::regclass AND contype = 'f'}), 'k_pid_fkey',
	'which has its key again');
is(sql(q{SELECT count(*) FROM app.pv}), '3', 'a view of a table made again reads it');
is(sql(q{SELECT n FROM app.pm}), '3', 'and a materialized view of it is refreshed');

move_to($a, 'back to a root holding none');
is(deferred(), '', 'nothing is set aside');
is(sql(q{SELECT string_agg(conname, ',' ORDER BY conname) FROM pg_constraint
	WHERE conrelid IN ('app.p'::regclass, 'app.c'::regclass) AND contype IN ('u', 'f')}),
	'c_code_fkey,code_one', 'the unique constraint and the key are on their tables');
is(sql(q{SELECT test_ws_violations(test_capture_barrier('app'))}), '', 'nothing is violated');

# From one root holding violations to another: what the first set aside is back, and a check the
# second adds is set aside of a table it changes.
move_to($m1, 'to a root holding violations again');
my $m2 = merged($a, q{ALTER TABLE t ADD CONSTRAINT v_pos CHECK (v > 0)}, q{INSERT INTO t VALUES (2, -1)});
is(sql(qq{SELECT test_ws_violations('$m2')}), 'check t v_pos 2', 'the second merge holds a row its check refuses');
%oid = map { $_ => oid_of("app.$_") } qw(p c t other);
move_to($m2, 'to another root holding violations');
is(deferred(), 't|c|v_pos', 'only the check is set aside');
is(sql(q{SELECT count(*) FROM pg_constraint WHERE conrelid = 'app.t'::regclass AND conname = 'v_pos'}), '0',
	'and it is not on its table');
is(sql(q{SELECT string_agg(conname, ',' ORDER BY conname) FROM pg_constraint
	WHERE conrelid IN ('app.p'::regclass, 'app.c'::regclass) AND contype IN ('u', 'f')}),
	'c_code_fkey,code_one', 'what the first set aside is on its tables');
isnt(oid_of('app.t'), $oid{t}, 'a table changed with a check set aside is made again, not altered');
is(oid_of('app.other'), $oid{other}, 'a table with nothing set aside either side is kept');

# What is set aside resolves once the rows are fixed, as a restore's does.
sql(q{DELETE FROM app.t WHERE v < 0});
is(sql(q{SELECT test_ws_viol_resolve('app')}), '0', 'with the row fixed, nothing waits');
is(sql(q{SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'v_pos'}), 'CHECK ((v > 0))',
	'and the check is on its table');

done_testing();
