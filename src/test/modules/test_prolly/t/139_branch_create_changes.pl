# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch made from one with changes not committed, as Dolt makes one: by default at the head, the
# changes left where they were made; taking them, with the changes, and the one they came from reset
# to its head; and at an older commit, or from another branch at its head.  Each holds exactly the
# root it's made at, a schema made since dropped and one dropped since made again, every index and
# heap right, and capture has each from there.  Taking changes at a commit, or a commit not there,
# is refused.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_changes');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION amcheck');

my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on { return "SET catalog_branch = $_[0]; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n"; }
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', on($branch) . $sql);
}
sub rows { return sql($_[0], q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t}); }
sub root { return sql($_[0], "SELECT test_branch_barrier($_[0])"); }
sub schemas { return sql($_[0], q{SELECT string_agg(nspname, ',' ORDER BY nspname) FROM pg_namespace WHERE nspbranch <> 0}); }

# How many B-tree indexes are checked (amcheck raises for one that isn't right), what's wrong in each
# heap and TOAST table, and whether capture has each schema as its tables hold it.
sub checks
{
	my ($branch) = @_;
	my $indexes = sql($branch, q{SELECT count(*) FROM (SELECT c.oid FROM pg_class c JOIN pg_am a ON a.oid = c.relam
		JOIN pg_namespace n ON n.oid = c.relnamespace
		WHERE n.nspbranch <> 0 AND c.relkind = 'i' AND a.amname = 'btree' OFFSET 0) x
		WHERE bt_index_check(x.oid, true, true) IS NOT NULL});
	my $heaps = sql($branch, q{SELECT count(*) FROM (SELECT r.oid FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace,
		LATERAL (VALUES (c.oid), (nullif(c.reltoastrelid, 0))) r(oid)
		WHERE n.nspbranch <> 0 AND c.relkind IN ('r', 'm') AND r.oid IS NOT NULL OFFSET 0) x,
		LATERAL verify_heapam(x.oid)});
	my @off = grep {
		sql($branch, qq{SELECT test_capture_barrier('$_')}) ne sql($branch, qq{SELECT test_ws_root_capture('$_')})
	} split /,/, schemas($branch);
	return "$indexes indexes/$heaps wrong/" . (@off ? "capture off on @off" : 'capture right');
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner;
	CREATE SCHEMA keep VERSIONED; GRANT ALL ON SCHEMA keep TO app_owner;
	SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, v text); CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT g, 'c1' FROM generate_series(1, 1000) g;
	CREATE TABLE keep.k (id integer PRIMARY KEY); INSERT INTO keep.k VALUES (1)});
sql($main, "SELECT test_branch_link('main', $main)");
my $c1_root = sql($main, q{SELECT test_branch_commit('main', 'c1')});
my $c1 = sql($main, q{SELECT test_branch_head('main')});
my $c1_rows = rows($main);

# Changes not committed: rows, a column, a schema made and one dropped.
sub change
{
	sql($main, q{SET ROLE app_owner; UPDATE app.t SET v = 'dirty' WHERE id <= 10; DELETE FROM app.t WHERE id > 990;
		INSERT INTO app.t VALUES (2000, 'new'); ALTER TABLE app.t ADD COLUMN w integer;
		RESET ROLE; CREATE SCHEMA extra VERSIONED; GRANT ALL ON SCHEMA extra TO app_owner; SET ROLE app_owner;
		CREATE TABLE extra.e (id integer PRIMARY KEY); INSERT INTO extra.e VALUES (7);
		RESET ROLE; DROP SCHEMA keep CASCADE});
}
change();
my $dirty_root = root($main);
my $dirty_rows = rows($main);
isnt($dirty_root, $c1_root, 'main has changes not committed');

# By default: at the head, the changes left on main.
my $clean = sql($main, q{SELECT test_branch_create('clean', 'main')});
is(root($clean), $c1_root, 'a branch made from one with changes is at its head');
is(rows($clean), $c1_rows, 'with its head\'s rows');
is(schemas($clean), 'app,keep', 'a schema made since dropped, and one dropped since made again');
is(sql($clean, q{SELECT count(*) FROM pg_attribute WHERE attrelid = 'app.t'::regclass AND attname = 'w'}), '0',
	'a column added since not there');
is(checks($clean), '3 indexes/0 wrong/capture right', 'every index and heap right, and capture with them');
is(root($main), $dirty_root, 'and main keeps its changes');
is(rows($main), $dirty_rows, 'its rows as they were');
is(test_head('clean'), $c1, 'made at main\'s head');

# Taking the changes: the branch has them, and main is reset to its head.
my $moved = sql($main, q{SELECT test_branch_create('moved', 'main', NULL, true)});
is(root($moved), $dirty_root, 'a branch taking another\'s changes has them');
is(rows($moved), $dirty_rows, 'with their rows');
is(schemas($moved), 'app,extra', 'and their schemas');
is(checks($moved), '3 indexes/0 wrong/capture right', 'every index and heap right, and capture with them');
is(root($main), $c1_root, 'and the one they came from is reset to its head');
is(rows($main), $c1_rows, 'its rows the head\'s');
is(schemas($main), 'app,keep', 'its schemas too');
is(checks($main), '3 indexes/0 wrong/capture right', 'every index and heap right there, and capture with them');
is(test_head('moved'), $c1, 'made at main\'s head');

# At an older commit; and from another branch, at its head, that branch's changes left on it.
change();
sql($main, q{SELECT test_branch_commit('main', 'c2')});
my $c2_root = root($main);
my $old = sql($main, qq{SELECT test_branch_create('old', 'main', '$c1')});
is(root($old), $c1_root, 'a branch made at an older commit holds its root');
is(rows($old), $c1_rows, 'with its rows');
is(schemas($old), 'app,keep', 'and its schemas, one dropped since made again');
is(checks($old), '3 indexes/0 wrong/capture right', 'every index and heap right, and capture with them');
is(test_head('old'), $c1, 'made at that commit');
is(root($main), $c2_root, 'main as it was');
my $copy = sql($moved, q{SELECT test_branch_create('copy', 'moved')});
is(root($copy), $c1_root, 'a branch made from another branch is at that one\'s head');
is(root($moved), $dirty_root, 'which keeps its changes');
is(checks($copy), '3 indexes/0 wrong/capture right', 'every index and heap right, and capture with them');

# Refused.
my ($rc, $out, $err) = $node->psql('postgres', qq{SELECT test_branch_create('x', 'main', '$c1', true)});
like($err, qr/a branch made at a commit can't take another's changes/, 'taking changes at a commit is refused');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_create('x', 'main', '0000000000000000000000000000000000000000')});
like($err, qr/commit 0+ not found/, 'and a commit not there');

sub test_head { return $node->safe_psql('postgres', qq{SELECT test_branch_head('$_[0]')}); }

done_testing();
