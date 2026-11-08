# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch made from another's head: its catalog rows copied, each table, index, TOAST table and
# materialized view sharing every page of its parent's, its sequences where the parent's were.  It
# holds the parent's root, and capture has it from there.  Each branch then writes its own rows, and
# neither sees the other's; every index stays right on both, and each heap, values stored out of
# line too (t/141 has more of those).  A writer open on the parent is waited out, and the branch is
# made after it, at the head, whether it commits or aborts; so is DDL on the parent, and the branch
# has what it made.  A branch made from one not there, or under a name taken, is refused; one made
# in a transaction rolled back is not there, and one committed outlasts a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_create');
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
sub rows { return sql($_[0], q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t}); }

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
			WHERE c.relnamespace = 'app'::regnamespace AND c.relkind IN ('r', 'm') AND r.oid IS NOT NULL OFFSET 0) x,
			LATERAL verify_heapam(x.oid)});
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, v text, big text);
	ALTER TABLE app.t ALTER COLUMN big SET STORAGE EXTERNAL;
	CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT g, 'v' || g, NULL FROM generate_series(1, 2000) g;
	CREATE SEQUENCE app.s; SELECT setval('app.s', 42);
	CREATE MATERIALIZED VIEW app.seen AS SELECT count(*) AS n FROM app.t});
sql($main, "SELECT test_branch_link('main', $main)");
my $root = sql($main, q{SELECT test_branch_commit('main', 'c1')});

# Made from main's head.
my $feature = sql($main, q{SELECT test_branch_create('feature', 'main')});
is(sql($main, q{SELECT test_branch_of('feature')}), $feature, 'a branch made is linked to its catalog branch');
is($node->safe_psql('postgres', on($main) . q{SET lock_timeout = '7s'; SELECT test_branch_create('timeout_kept', 'main') > 0;
	SHOW lock_timeout}), "t\n7s", 'the session\'s lock_timeout is its own after a branch is made');
is(sql($feature, "SELECT test_branch_barrier($feature)"), $root, 'it holds its parent\'s root');
is(rows($feature), rows($main), 'with its rows');
is(sql($feature, q{SELECT last_value FROM app.s}), '42', 'and its sequence where the parent\'s was');
is(sql($feature, q{SELECT n FROM app.seen}), '2000', 'and its materialized view');
for my $rel ('app.t', 'app.t_pkey', 'app.t_v', 'app.seen')
{
	my $src = sql($main, qq{SELECT pg_relation_filenode('$rel')});
	my $dst = sql($feature, qq{SELECT pg_relation_filenode('$rel')});
	isnt($dst, $src, "$rel: the branch's is a relation of its own");
	is(sql($main, qq{SELECT count(*) FROM test_bmap_diff($src, $dst)}), '0', "$rel: sharing every page");
}
my $toast_src = sql($main, q{SELECT pg_relation_filenode(reltoastrelid) FROM pg_class WHERE oid = 'app.t'::regclass});
my $toast_dst = sql($feature, q{SELECT pg_relation_filenode(reltoastrelid) FROM pg_class WHERE oid = 'app.t'::regclass});
is(sql($main, qq{SELECT count(*) FROM test_bmap_diff($toast_src, $toast_dst)}), '0', 'and its TOAST table');

# Each writes its own, and sees only its own; capture follows each.
sql($feature, q{UPDATE app.t SET v = 'feature' WHERE id <= 3; DELETE FROM app.t WHERE id = 2000;
	INSERT INTO app.t VALUES (3000, 'only feature', NULL); SELECT nextval('app.s')});
sql($main, q{UPDATE app.t SET v = 'main' WHERE id = 1; INSERT INTO app.t VALUES (4000, 'only main', NULL)});
is(sql($feature, q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t WHERE id IN (1, 2, 4000, 3000, 2000)}),
	'1:feature,2:feature,3000:only feature', 'the branch sees its own writes, not its parent\'s');
is(sql($main, q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t WHERE id IN (1, 2, 4000, 3000, 2000)}),
	'1:main,2:v2,2000:v2000,4000:only main', 'and the parent its own');
is(sql($feature, q{SELECT v FROM app.t WHERE v = 'only feature'}), 'only feature', 'found through the branch\'s index');
is(sql($main, q{SELECT count(*) FROM app.t WHERE v = 'only feature'}), '0', 'and not through the parent\'s');
is(sql($main, q{SELECT last_value FROM app.s}), '42', 'a sequence goes its own way on each');
is(checks($feature), '2/0', 'every index and heap of the branch is right');
is(checks($main), '2/0', 'and of the parent');
is(sql($feature, q{SELECT test_capture_barrier('app')}), sql($feature, q{SELECT test_ws_root_capture('app')}),
	'capture has the branch\'s writes');
is(sql($main, q{SELECT test_capture_barrier('app')}), sql($main, q{SELECT test_ws_root_capture('app')}),
	'and the parent\'s');

# Values stored out of line after the branch is made are each branch's own.
sql($feature, q{SET ROLE app_owner; INSERT INTO app.t VALUES (7001, 'big', repeat(md5('feature'), 300))});
sql($main, q{SET ROLE app_owner; INSERT INTO app.t VALUES (7001, 'big', repeat(md5('main'), 300))});
sql($feature, q{DELETE FROM app.t WHERE id = 7001});
is(sql($main, q{SELECT big = repeat(md5('main'), 300) FROM app.t WHERE id = 7001}), 't',
	'a value stored out of line after the branch is made reads on the parent, the branch\'s deleted');
is(checks($main), '2/0', 'and every index and heap of the parent is right');
sql($main, q{DELETE FROM app.t WHERE id = 7001});
my $fc = sql($feature, q{SELECT test_branch_commit('feature', 'f1')});
is(sql($feature, "SELECT test_branch_barrier($feature)"), $fc, 'the branch commits');

# A writer open on the parent: waited out, and the branch made after it, as it ends.
sql($main, q{SELECT test_branch_commit('main', 'c2')});
for my $case (['COMMIT', 5000], ['ROLLBACK', 5001])
{
	my ($ending, $id) = @$case;
	my $writer = $node->background_psql('postgres');
	$writer->query_safe(on($main));
	$writer->query_safe(qq{BEGIN; INSERT INTO app.t VALUES ($id, 'open', NULL)});
	my $wpid = $writer->query_safe('SELECT pg_backend_pid()');
	my $maker = $node->background_psql('postgres', on_error_stop => 0);
	$maker->query_safe(on($main));
	my $name = lc "after_$ending";
	$maker->query_until(qr/started/, "\\echo started\nSELECT test_branch_create('$name', 'main');\n\\echo made\n");
	$node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity
		WHERE query LIKE 'SELECT test_branch_create%' AND $wpid = ANY (pg_blocking_pids(pid))})
	  or die 'the branch did not wait for the writer';
	pass("a branch waits for a writer open on its parent ($ending)");
	$writer->query_safe($ending);
	$writer->quit;
	my $out = $maker->query_until(qr/made/, '');
	is($maker->{stderr}, '', "and is made once it ends ($ending)");
	$maker->quit;
	my ($made) = $out =~ /^(\d+)$/m;
	is(sql($made, qq{SELECT count(*) FROM app.t WHERE id = $id}), '0',
		"without the writer's row, at the head ($ending)");
	is(sql($main, qq{SELECT count(*) FROM app.t WHERE id = $id}), $ending eq 'COMMIT' ? '1' : '0',
		"which the parent has if it committed ($ending)");
}

# DDL open on the parent: waited out, and the branch has what it made.
my $ddl = $node->background_psql('postgres');
$ddl->query_safe(on($main));
$ddl->query_safe(q{BEGIN; SET ROLE app_owner; CREATE TABLE app.late (id integer PRIMARY KEY)});
my $dpid = $ddl->query_safe('SELECT pg_backend_pid()');
my $maker = $node->background_psql('postgres', on_error_stop => 0);
$maker->query_safe(on($main));
$maker->query_until(qr/started/, "\\echo started\nSELECT test_branch_create('after_ddl', 'main', NULL, true);\n\\echo made\n");
ok($node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity
	WHERE query LIKE 'SELECT test_branch_create%' AND $dpid = ANY (pg_blocking_pids(pid))}),
	'a branch waits for DDL open on its parent');
$ddl->query_safe('COMMIT');
$ddl->quit;
my $out = $maker->query_until(qr/made/, '');
is($maker->{stderr}, '', 'and is made once it commits');
$maker->quit;
my ($after_ddl) = $out =~ /^(\d+)$/m;
is(sql($after_ddl, q{SELECT to_regclass('app.late') IS NOT NULL}), 't', 'with the table it made, its changes taken');
is(sql($main, q{SELECT to_regclass('app.late') IS NULL}), 't', 'which the parent, reset to its head, no longer has');

# A schema being made on the parent: waited out, and the branch has it.
$ddl = $node->background_psql('postgres');
$ddl->query_safe(on($main));
$ddl->query_safe(q{BEGIN; CREATE SCHEMA newer VERSIONED});
$dpid = $ddl->query_safe('SELECT pg_backend_pid()');
$maker = $node->background_psql('postgres', on_error_stop => 0);
$maker->query_safe(on($main));
$maker->query_until(qr/started/, "\\echo started\nSELECT test_branch_create('after_schema', 'main', NULL, true);\n\\echo made\n");
ok($node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity
	WHERE query LIKE 'SELECT test_branch_create%' AND $dpid = ANY (pg_blocking_pids(pid))}),
	'a branch waits for a schema being made on its parent');
$ddl->query_safe('COMMIT');
$ddl->quit;
$out = $maker->query_until(qr/made/, '');
is($maker->{stderr}, '', 'and is made once it commits');
$maker->quit;
my ($after_schema) = $out =~ /^(\d+)$/m;
is(sql($after_schema, q{SELECT count(*) FROM pg_namespace WHERE nspname = 'newer' AND nspbranch <> 0}), '1',
	'with the schema, its changes taken');

# A writer that goes on to DDL as the branch waits for it: the branch gives way, and the writer's
# transaction is never what fails.
my $wd = $node->background_psql('postgres', on_error_stop => 0);
$wd->query_safe(on($main));
$wd->query_safe(q{BEGIN; SET ROLE app_owner; INSERT INTO app.t VALUES (9001, 'then DDL', NULL)});
my $wdpid = $wd->query_safe('SELECT pg_backend_pid()');
$maker = $node->background_psql('postgres', on_error_stop => 0);
$maker->query_safe(on($main));
$maker->query_until(qr/started/, "\\echo started\nSELECT test_branch_create('gave_way', 'main');\n\\echo made\n");
ok($node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity
	WHERE query LIKE 'SELECT test_branch_create%' AND $wdpid = ANY (pg_blocking_pids(pid))}),
	'a branch waits for a writer open on its parent');
$wd->query_safe(q{CREATE INDEX t_late ON app.t (id, v)});
is($wd->{stderr}, '', 'which goes on to DDL, and isn\'t what fails');
$wd->query_safe('COMMIT');
$wd->quit;
$out = $maker->query_until(qr/made/, '');
is($maker->{stderr}, '', 'and the branch is made once it commits');
$maker->quit;
my ($gave_way) = $out =~ /^(\d+)$/m;
is(sql($gave_way, q{SELECT count(*) FROM pg_class WHERE relname = 't_late'}) . sql($main, q{SELECT count(*) FROM pg_class WHERE relname = 't_late'}),
	'01', 'at the head, the index the writer made left on the parent as its change');

# Refused.
my ($rc, $err);

($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_create('x', 'nowhere')});
like($err, qr/branch "nowhere" not found/, 'a branch from one not there is refused');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_create('feature', 'main')});
like($err, qr/a branch named "feature" already exists/, 'and under a name taken');

# Rolled back: not there.
my $before = $node->safe_psql('postgres', q{SELECT count(*) FROM pg_namespace WHERE nspname = 'app'});
($rc, $out, $err) = $node->psql('postgres', on($main) . q{BEGIN; SELECT test_branch_create('gone', 'main'); ROLLBACK;});
is($rc, 0, 'a branch made in a transaction rolled back') or diag($err);
is(sql($main, q{SELECT test_branch_of('gone') IS NULL}), 't', 'is not linked');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_namespace WHERE nspname = 'app'}), $before, 'and has no schema');

# Committed: outlasts a crash.
my $feature_rows = rows($feature);
$node->stop('immediate');
$node->start;
is(rows($feature), $feature_rows, 'after a crash, the branch reads as it did');
# (a sequence is ahead of where it was after a crash, as it logs values before it hands them out)
is(sql($feature, q{SELECT test_capture_barrier('app')}), sql($feature, q{SELECT test_ws_root_capture('app')}),
	'and capture has what its tables hold');
is(checks($feature), '2/0', 'every index and heap right');

done_testing();
