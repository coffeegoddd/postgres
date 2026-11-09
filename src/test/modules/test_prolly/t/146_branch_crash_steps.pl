# Copyright (c) 2026, PostgreSQL Global Development Group

# A crash at each step of making a branch, giving one a working set, giving one back, deleting one
# and renaming one onto another: the server is stopped at once while a backend waits between two of
# the step's parts.  After it, the branch is whole, as it was before, or not there at all, and the
# branch it came from keeps its rows, and any changes it was to give up.  Nothing leaks: once the
# storage of transactions that never committed is dropped and the release queue drained, a sweep
# frees no block and corrects no count, and every map belongs to a relation some branch's catalog
# names.  The same holds after each commits and the server crashes, and a delete's pages are freed
# then.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('branch_crash_steps');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\nrestart_after_crash = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION amcheck; CREATE EXTENSION injection_points');

my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on { return "SET catalog_branch = $_[0]; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n"; }
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', on($branch) . $sql);
}
sub rows { return sql($_[0], q{SELECT string_agg(id || ':' || v || ':' || length(coalesce(big, '')), ',' ORDER BY id) FROM app.t}); }
my $schemas = q{SET catalog_branch_filter = off; SELECT count(*) FROM pg_namespace WHERE nspbranch <> 0};

# How many B-tree indexes are checked (amcheck raises for one that isn't right), and what's wrong in
# each heap and TOAST table.
sub checks
{
	my ($branch) = @_;
	return sql($branch, q{SELECT count(*) FROM (SELECT c.oid FROM pg_class c JOIN pg_am a ON a.oid = c.relam
			WHERE c.relnamespace = 'app'::regnamespace AND c.relkind = 'i' AND a.amname = 'btree' OFFSET 0) x
			WHERE bt_index_check(x.oid, true, true) IS NOT NULL})
	  . '/' . sql($branch, q{SELECT count(*) FROM (SELECT r.oid FROM pg_class c,
			LATERAL (VALUES (c.oid), (nullif(c.reltoastrelid, 0))) r(oid)
			WHERE c.relnamespace = 'app'::regnamespace AND c.relkind = 'r' AND r.oid IS NOT NULL OFFSET 0) x,
			LATERAL verify_heapam(x.oid)});
}

# The storage of transactions that never committed dropped, as autovacuum drops it after a start,
# and what was freed freed: then what a sweep frees and puts right, which is a block nothing holds,
# and the maps no catalog row of any branch names.
sub leaks
{
	$node->safe_psql('postgres', q{SELECT test_bmap_drop_orphans()});
	$node->safe_psql('postgres', q{SELECT * FROM test_bmap_release_drain()});
	return $node->safe_psql('postgres', q{SELECT freed || ' freed, ' || recounted || ' recounted' FROM test_bmap_sweep()})
	  . ', ' . $node->safe_psql('postgres', q{SET catalog_branch_filter = off;
		SELECT count(*) || ' maps unnamed' FROM (SELECT 'base/' || d.oid AS dir FROM pg_database d
			WHERE d.datname = current_database()) d, pg_ls_dir(d.dir) f
		WHERE f ~ '^\d+_bmap$'
		  AND NOT EXISTS (SELECT FROM pg_class c WHERE pg_relation_filenode(c.oid)::text = split_part(f, '_', 1))});
}
my $none = '0 freed, 0 recounted, 0 maps unnamed';

# A branch whole: its head, its working set linked to it, with rows, indexes and heaps right, and
# capture following it.
sub whole
{
	my ($name, $head, $rows) = @_;
	my $branch = $node->safe_psql('postgres', qq{SELECT test_branch_of('$name')});
	return 0 if $branch eq '' || $node->safe_psql('postgres', qq{SELECT test_branch_head('$name')}) ne $head;
	return rows($branch) eq $rows && checks($branch) eq '2/0'
	  && sql($branch, q{SELECT test_capture_barrier('app')}) eq sql($branch, q{SELECT test_ws_root_capture('app')});
}

# A row written on a branch and committed: the head moves, to the root its tables hold.
sub commits
{
	my ($name) = @_;
	my $branch = $node->safe_psql('postgres', qq{SELECT test_branch_of('$name')});
	my $was = $node->safe_psql('postgres', qq{SELECT test_branch_head('$name')});
	sql($branch, qq{SET ROLE app_owner; INSERT INTO app.t VALUES (9000, '$name', NULL)});
	my $root = sql($branch, qq{SELECT test_branch_commit('$name', 'after')});
	return $node->safe_psql('postgres', qq{SELECT test_branch_head('$name')}) ne $was
	  && sql($branch, "SELECT test_dbroot_schemas('$root') = 'app=' || test_ws_root_capture('app')") eq 't';
}

# Not at all: no head, no working set, no catalog rows.
sub absent
{
	my ($name, $base) = @_;
	return $node->safe_psql('postgres', qq{SELECT test_branch_head('$name') IS NULL AND test_branch_of('$name') IS NULL})
	  eq 't' && $node->safe_psql('postgres', $schemas) eq $base;
}

# Stopped at once while a backend running statement on main waits at point.
sub crash_at
{
	my ($point, $statement) = @_;
	$node->safe_psql('postgres', qq{SELECT injection_points_attach('$point', 'wait')});
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->query_until(qr/started/, on($main) . "\\echo started\n$statement;\n");
	$node->wait_for_event('client backend', $point);
	$node->stop('immediate');
	eval { $s->quit };
	$node->start;
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, v text, big text);
	ALTER TABLE app.t ALTER COLUMN big SET STORAGE EXTERNAL;
	CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT g, 'c1', CASE WHEN g % 100 = 0 THEN repeat(md5(g::text), 100) END
		FROM generate_series(1, 2000) g});
sql($main, "SELECT test_branch_link('main', $main)");
sql($main, q{SELECT test_branch_commit('main', 'c1')});
my $c1 = $node->safe_psql('postgres', q{SELECT test_branch_head('main')});
my $c1_rows = rows($main);
is(leaks(), $none, 'before any crash, nothing leaked');

# One to take changes from: changes not committed, on pages of its own and shared.
my $src = sql($main, q{SELECT test_branch_create('src', 'main')});
sql($src, q{SET ROLE app_owner; UPDATE app.t SET v = 'src' WHERE id <= 300; INSERT INTO app.t VALUES (5000, 'src', repeat('x', 5000))});
my $src_rows = rows($src);
my $base = $node->safe_psql('postgres', $schemas);

# Made: at each step, not there after the crash.
for my $point (qw(branch-share-held branch-share-copied branch-share-moved branch-share-linked))
{
	crash_at($point, q{SELECT test_branch_create('made', 'main')});
	ok(absent('made', $base), "made, crashed at $point: the branch isn't there");
	is(rows($main), $c1_rows, "$point: its parent's rows as they were");
	is(leaks(), $none, "$point: nothing leaked");
}

# Made from one with changes not committed, at its head: its tables written back to the head as it's
# made, then not there, and the parent's changes where they were.
for my $point (qw(branch-share-held branch-share-copied branch-share-moved branch-share-linked))
{
	crash_at($point, q{SELECT test_branch_create('fresh', 'src')});
	ok(absent('fresh', $base), "made from one with changes, crashed at $point: the branch isn't there");
	is(rows($src), $src_rows, "$point: its parent's changes where they were");
	is(leaks(), $none, "$point: nothing leaked");
}

# Made with the changes taken: not there, and the parent's changes where they were.
for my $point (qw(branch-share-held branch-share-copied branch-share-moved branch-share-linked))
{
	crash_at($point, q{SELECT test_branch_create('taker', 'src', NULL, true)});
	ok(absent('taker', $base), "made with changes taken, crashed at $point: the branch isn't there");
	is(rows($src), $src_rows, "$point: its parent's changes where they were");
	is(checks($src), '2/0', "$point: every index and heap of the parent right");
	is(leaks(), $none, "$point: nothing leaked");
}

# Given a working set: a head still, alone.
sql($main, q{SELECT test_branch_make('head', 'main')});
for my $point (qw(branch-share-held branch-share-copied branch-share-moved branch-share-linked))
{
	crash_at($point, q{SELECT test_branch_materialize('head')});
	is($node->safe_psql('postgres', q{SELECT test_branch_head('head') = test_branch_head('main') AND test_branch_of('head') IS NULL}),
		't', "given a working set, crashed at $point: a head still");
	is($node->safe_psql('postgres', $schemas), $base, "$point: with no catalog rows");
	is(leaks(), $none, "$point: nothing leaked");
}

# Committed, each is whole after a crash, and commits.
sql($main, q{SELECT test_branch_create('made', 'main')});
sql($main, q{SELECT test_branch_create('fresh', 'src')});
sql($main, q{SELECT test_branch_create('taker', 'src', NULL, true)});
sql($main, q{SELECT test_branch_materialize('head')});
sql($main, q{SELECT test_branch_create('r1', 'main')});
sql($main, q{SELECT test_branch_create('r2', 'main')});
sql($main, q{SELECT test_branch_make('alone', 'main')});
$node->stop('immediate');
$node->start;
ok(whole('made', $c1, $c1_rows), 'made and committed, a branch is whole after a crash');
ok(whole('fresh', $c1, $c1_rows), 'and one made from one with changes, at its head');
ok(whole('taker', $c1, $src_rows), 'and one made with changes taken');
ok(whole('src', $c1, $c1_rows), 'the one they came from at its head');
ok(whole('head', $c1, $c1_rows), 'and one given a working set');
is(leaks(), $none, 'nothing leaked');
ok(commits('made') && commits('taker'), 'each commits, its head moving to what its tables hold');
my $made_rows = rows($node->safe_psql('postgres', q{SELECT test_branch_of('made')}));
my $made_head = $node->safe_psql('postgres', q{SELECT test_branch_head('made')});
my $taker_rows = rows($node->safe_psql('postgres', q{SELECT test_branch_of('taker')}));
my $taker_head = $node->safe_psql('postgres', q{SELECT test_branch_head('taker')});

# Given back: at the step, whole after the crash.
crash_at('branch-give-back-dropped', q{SELECT test_branch_dematerialize('head')});
ok(whole('head', $c1, $c1_rows), 'given back, crashed as its catalog rows are dropped: the branch whole');
is(leaks(), $none, 'nothing leaked');

# Deleted, changes not committed and all: at each step, whole after the crash.
for my $point (qw(branch-give-back-dropped branch-delete-removed))
{
	sql($node->safe_psql('postgres', q{SELECT test_branch_of('taker')}), q{SET ROLE app_owner; UPDATE app.t SET v = 'dirty' WHERE id = 9000});
	crash_at($point, q{SELECT test_branch_delete('taker', true)});
	ok(whole('taker', $taker_head, $taker_rows =~ s/9000:taker/9000:dirty/r),
		"deleted, crashed at $point: the branch whole, its changes too");
	is(leaks(), $none, "$point: nothing leaked");
}

# A head alone deleted: there still after a crash.
crash_at('branch-delete-removed', q{SELECT test_branch_delete('alone', true)});
is($node->safe_psql('postgres', q{SELECT test_branch_head('alone') = test_branch_head('main') AND test_branch_of('alone') IS NULL}),
	't', 'a head alone deleted, crashed as it\'s removed: there still');
is(leaks(), $none, 'nothing leaked');

# Renamed onto another, forced, which deletes that one: at each step, both whole after the crash.
for my $point (qw(branch-give-back-dropped branch-delete-removed))
{
	crash_at($point, q{SELECT test_branch_rename('r1', 'r2', true)});
	ok(whole('r1', $c1, $c1_rows) && whole('r2', $c1, $c1_rows),
		"renamed onto another, crashed at $point: both whole");
	is(leaks(), $none, "$point: nothing leaked");
}

# Committed: not there after a crash, the pages they alone held freed then.
my $gone = join(', ', map { $node->safe_psql('postgres', qq{SELECT test_branch_of('$_')}) } qw(taker head r2));
sql($main, q{SELECT test_branch_delete('taker', true)});
sql($main, q{SELECT test_branch_dematerialize('head')});
sql($main, q{SELECT test_branch_rename('r1', 'r2', true)});
sql($main, q{SELECT test_branch_delete('alone', true)});
$node->stop('immediate');
$node->start;
my $pending = $node->safe_psql('postgres', q{SELECT test_bmap_release_pending()});
is($node->safe_psql('postgres', q{SELECT test_branch_head('taker') IS NULL AND test_branch_of('taker') IS NULL
	AND test_branch_head('alone') IS NULL}), 't', 'deleted and committed, a branch isn\'t there after a crash');
is($node->safe_psql('postgres', q{SELECT test_branch_head('head') IS NOT NULL AND test_branch_of('head') IS NULL}), 't',
	'one given back is a head');
ok(whole('r2', $c1, $c1_rows) && $node->safe_psql('postgres', q{SELECT test_branch_head('r1') IS NULL}) eq 't',
	'and one renamed onto another is whole by its name');
is($node->safe_psql('postgres', qq{SET catalog_branch_filter = off; SELECT count(*) FROM pg_namespace WHERE nspbranch IN ($gone)}),
	'0', 'the catalog rows of those deleted and given back gone');
is(leaks(), $none, 'nothing leaked');
cmp_ok($pending, '>', 0, 'the pages they alone held queued to be freed');
ok(whole('made', $made_head, $made_rows) && whole('fresh', $c1, $c1_rows) && whole('src', $c1, $c1_rows), 'the others whole');
is(rows($main), $c1_rows, 'and their parent');

done_testing();
