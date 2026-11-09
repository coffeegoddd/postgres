# Copyright (c) 2026, PostgreSQL Global Development Group

# A crash at each step of making a branch, giving one a working set, giving one back and deleting
# one, the server stopped at once while a backend waits between two of the step's parts: the branch
# is there whole, as it was before, or not at all, and nothing leaks; once the storage of
# transactions that never committed is dropped and what was freed is, a sweep finds no block nothing
# holds, and no count wrong.  So too after each commits, the pages a delete frees freed after the
# crash.  The branch's parent keeps its rows, and its changes not committed when they were to be
# taken.

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
		WHERE f ~ '^\d+_bmap$' AND (pg_stat_file(d.dir || '/' || f)).size > 0
		  AND NOT EXISTS (SELECT FROM pg_class c WHERE pg_relation_filenode(c.oid)::text = split_part(f, '_', 1))});
}
my $none = '0 freed, 0 recounted, 0 maps unnamed';
sub in_use { return $node->safe_psql('postgres', q{SELECT test_bmap_used_blocks()}); }

# A branch whole: its head, its working set linked to it, with rows, indexes and heaps right.
sub whole
{
	my ($name, $head, $rows) = @_;
	my $branch = $node->safe_psql('postgres', qq{SELECT test_branch_of('$name')});
	return 0 if $branch eq '' || $node->safe_psql('postgres', qq{SELECT test_branch_head('$name')}) ne $head;
	return rows($branch) eq $rows && checks($branch) eq '2/0';
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

# Made with the changes taken: not there, and the parent's changes where they were.
for my $point (qw(branch-share-copied branch-share-moved branch-share-linked))
{
	crash_at($point, q{SELECT test_branch_create('taker', 'src', NULL, true)});
	ok(absent('taker', $base), "made with changes taken, crashed at $point: the branch isn't there");
	is(rows($src), $src_rows, "$point: its parent's changes where they were");
	is(checks($src), '2/0', "$point: every index and heap of the parent right");
	is(leaks(), $none, "$point: nothing leaked");
}

# Given a working set: a head still, alone.
sql($main, q{SELECT test_branch_make('head', 'main')});
for my $point (qw(branch-share-copied branch-share-moved branch-share-linked))
{
	crash_at($point, q{SELECT test_branch_materialize('head')});
	is($node->safe_psql('postgres', q{SELECT test_branch_head('head') = test_branch_head('main') AND test_branch_of('head') IS NULL}),
		't', "given a working set, crashed at $point: a head still");
	is($node->safe_psql('postgres', $schemas), $base, "$point: with no catalog rows");
	is(leaks(), $none, "$point: nothing leaked");
}

# Committed, each is whole after a crash.
sql($main, q{SELECT test_branch_create('made', 'main')});
sql($main, q{SELECT test_branch_create('taker', 'src', NULL, true)});
sql($main, q{SELECT test_branch_materialize('head')});
$node->stop('immediate');
$node->start;
ok(whole('made', $c1, $c1_rows), 'made and committed, a branch is whole after a crash');
ok(whole('taker', $c1, $src_rows), 'and one made with changes taken');
ok(whole('src', $c1, $c1_rows), 'the one they came from at its head');
ok(whole('head', $c1, $c1_rows), 'and one given a working set');
is(leaks(), $none, 'nothing leaked');
$base = $node->safe_psql('postgres', $schemas);

# Given back: at the step, whole after the crash.
crash_at('branch-give-back-dropped', q{SELECT test_branch_dematerialize('head')});
ok(whole('head', $c1, $c1_rows), 'given back, crashed as its catalog rows are dropped: the branch whole');
is(leaks(), $none, 'nothing leaked');

# Deleted, changes not committed and all: at each step, whole after the crash.
for my $point (qw(branch-give-back-dropped branch-delete-removed))
{
	crash_at($point, q{SELECT test_branch_delete('taker', true)});
	ok(whole('taker', $c1, $src_rows), "deleted, crashed at $point: the branch whole, its changes too");
	is(leaks(), $none, "$point: nothing leaked");
}

# Committed: not there after a crash, the pages it alone held freed then.
my $in_use = in_use();
sql($main, q{SELECT test_branch_delete('taker', true)});
sql($main, q{SELECT test_branch_dematerialize('head')});
$node->stop('immediate');
$node->start;
ok(absent('taker', $base - 2), 'deleted and committed, a branch isn\'t there after a crash');
is($node->safe_psql('postgres', q{SELECT test_branch_head('head') IS NOT NULL AND test_branch_of('head') IS NULL}), 't',
	'and one given back is a head, their catalog rows gone');
is(leaks(), $none, 'nothing leaked');
cmp_ok(in_use(), '<', $in_use, 'the pages they alone held freed');
ok(whole('made', $c1, $c1_rows) && whole('src', $c1, $c1_rows), 'the others whole');
is(rows($main), $c1_rows, 'and their parent');

done_testing();
