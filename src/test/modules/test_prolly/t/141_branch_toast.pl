# Copyright (c) 2026, PostgreSQL Global Development Group

# Values stored out of line, on the pages a branch shares with its parent, name the parent's TOAST
# relation; each branch reads them from its own copy of it, whole and in slices, and deletes them from
# its own, which leaves the other's as they were.  So a value updated or deleted on the branch still
# reads on the parent, and one deleted on the parent and vacuumed away there still reads on the
# branch, and on a branch made from the branch, even once the middle one's table is dropped.  Every heap is right, its TOAST values checked too,
# capture has each branch's rows, and all of it outlasts a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_toast');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION amcheck');

my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on { return "SET catalog_branch = $_[0]; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n"; }
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', on($branch) . $sql);
}

# (each value its own: big ones stored out of line whole, compressed ones too)
sub value { my ($id, $tag) = @_; return "repeat(md5('$tag' || $id), 400)"; }
sub reads
{
	my ($branch, $id, $tag) = @_;
	return sql($branch, "SELECT big = " . value($id, $tag) . " AND substr(big, 5, 20) = substr(" . value($id, $tag)
		  . ", 5, 20) AND squeezed = " . value($id, "s$tag") . " FROM app.t WHERE id = $id");
}
sub heaps
{
	my ($branch) = @_;
	return sql($branch, q{SELECT count(*) FROM (SELECT r.oid FROM pg_class c,
		LATERAL (VALUES (c.oid), (nullif(c.reltoastrelid, 0))) r(oid)
		WHERE c.relnamespace = 'app'::regnamespace AND c.relkind = 'r' AND r.oid IS NOT NULL OFFSET 0) x,
		LATERAL verify_heapam(x.oid, check_toast => true)});
}
sub capture_right
{
	my ($branch) = @_;
	return sql($branch, q{SELECT test_capture_barrier('app') = test_ws_root_capture('app')});
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, big text, squeezed text);
	ALTER TABLE app.t ALTER COLUMN big SET STORAGE EXTERNAL});
sql($main, "SET ROLE app_owner; INSERT INTO app.t SELECT g, " . value('g', 'm') . ", " . value('g', 'sm')
	  . " FROM generate_series(1, 20) g");
sql($main, "SELECT test_branch_link('main', $main)");
sql($main, q{SELECT test_branch_commit('main', 'c1')});
ok(sql($main, q{SELECT count(*) FROM pg_toast.pg_toast_} . sql($main, q{SELECT 'app.t'::regclass::oid})) > 0,
	'main has values stored out of line');

# A branch of it reads them from its own copy of the TOAST relation.
my $feature = sql($main, q{SELECT test_branch_create('feature', 'main')});
is(reads($feature, 1, 'm') . reads($feature, 20, 'm'), 'tt', 'a branch reads its parent\'s values, whole and in slices');
isnt(sql($feature, q{SELECT reltoastrelid FROM pg_class WHERE oid = 'app.t'::regclass}),
	sql($main, q{SELECT reltoastrelid FROM pg_class WHERE oid = 'app.t'::regclass}), 'from a TOAST relation of its own');

# Changed and deleted on the branch: the parent's are as they were.
sql($feature, "SET ROLE app_owner; UPDATE app.t SET big = " . value(1, 'f') . ", squeezed = " . value(1, 'sf')
	  . " WHERE id = 1; DELETE FROM app.t WHERE id = 2");
is(reads($feature, 1, 'f'), 't', 'a value changed on the branch reads as changed there');
is(reads($main, 1, 'm') . reads($main, 2, 'm'), 'tt', 'and the parent\'s, changed and deleted on the branch, read as they were');

# Deleted on the parent, and vacuumed away there: the branch's are as they were.
sql($main, q{SET ROLE app_owner; DELETE FROM app.t WHERE id IN (3, 4)});
sql($main, q{VACUUM app.t});
is(reads($feature, 3, 'm') . reads($feature, 4, 'm'), 'tt', 'values deleted and vacuumed away on the parent read on the branch');

# A branch of the branch: the values its pages name are the first parent's, and its own copy has them.
my $deeper = sql($feature, q{SELECT test_branch_create('deeper', 'feature', NULL, true)});
is(reads($deeper, 3, 'm') . reads($deeper, 1, 'f'), 'tt', 'a branch of the branch reads them too, its parent\'s and theirs');
sql($deeper, q{SET ROLE app_owner; DELETE FROM app.t WHERE id = 3});
is(reads($feature, 3, 'm'), 't', 'and deletes them from its own copy alone');

# The middle one's table dropped: the branch made from it still reads what names the first parent.
sql($feature, q{SET ROLE app_owner; DROP TABLE app.t});
is(reads($deeper, 4, 'm'), 't', 'a branch\'s table dropped, the branch made from it still reads its values');
sql($feature, q{SET ROLE app_owner; CREATE TABLE app.t (id integer PRIMARY KEY, big text, squeezed text)});

# Every heap right, its values checked; capture with each.
is(heaps($main) . heaps($feature) . heaps($deeper), '000', 'every heap right, its values stored out of line too');
is(capture_right($feature) . capture_right($deeper), 'tt', 'and capture has each branch\'s rows');

# After a crash, the same.
$node->stop('immediate');
$node->start;
is(reads($deeper, 4, 'm') . reads($deeper, 1, 'f') . reads($main, 1, 'm'), 'ttt', 'after a crash, each reads as it did');

done_testing();
