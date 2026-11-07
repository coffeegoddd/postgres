# Copyright (c) 2026, PostgreSQL Global Development Group

# The happy path, end to end across the layers built so far, written as what a user does: make a
# versioned schema, write to it, commit, branch from that commit, write on both branches, commit
# both, see what changed, merge them, and have the merge as a schema with the rows of both.  Its
# tables are in block-mapped storage, its schemas and objects in branched catalogs, its roots in the
# history store, made by capture, and its diff and merge are the working set's.
#
# It goes through test functions where the operations aren't built yet; each such step says what's
# to replace it.  TODO marks what the operations still have to do.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('happy');
$node->init(allows_streaming => 'logical');

$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

# Two branches' catalogs, as making a branch will give.  TODO: the operations make these, and name them.
my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $feature = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $merged = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my %on = (main => "SET catalog_branch = $main;", feature => "SET catalog_branch = $feature;", merged => "SET catalog_branch = $merged;");

# (the working set's test functions read and write the history store here, not a backend's own)
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', "$on{$branch} SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n$sql");
}

# A commit on the branch, of what capture has of the schema as of now, and the root it holds.  TODO:
# dolt_commit(), on the version control branch the catalog branch is.
my %commits;
sub commit_root
{
	my ($branch) = @_;
	$commits{$branch}++;
	return sql($branch, qq{SELECT test_capture_commit_on('app', '$branch', '$branch-$commits{$branch}')});
}

# The root reading the branch's tables makes, which capture's must equal.
sub read_root
{
	my ($branch) = @_;
	return sql($branch, q{SELECT test_ws_root_capture('app')});
}

# A role whose objects can travel in a commit, and that a commit marks.
$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});

# 1. Make a versioned schema and write to it
sql('main', q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner; SET search_path = app;
	CREATE TABLE customers (id integer PRIMARY KEY, name text NOT NULL);
	CREATE TABLE orders (id integer PRIMARY KEY, customer integer REFERENCES customers, total numeric(10, 2));
	CREATE TABLE notes (body text);
	CREATE TABLE documents (id integer PRIMARY KEY, body text);
	CREATE SEQUENCE order_ids START 100;
	CREATE VIEW big_orders AS SELECT id, total FROM orders WHERE total > 100;
	INSERT INTO customers VALUES (1, 'ada'), (2, 'grace');
	INSERT INTO orders VALUES (1, 1, 50.00), (2, 2, 250.00);
	INSERT INTO notes VALUES ('first'), ('first');
	INSERT INTO documents VALUES (1, repeat('a long document. ', 2000));
	SELECT nextval('order_ids')});
is(sql('main', q{SELECT test_bmap_exists(pg_relation_filenode('app.orders'))}), 't', "the schema's tables are block-mapped");

# 2. Commit
my $first = commit_root('main');
like($first, qr/^[0-9a-f]{40}$/, 'a commit');
is($first, read_root('main'), 'of what the tables hold');

# 3. Branch from that commit.  TODO: a branch made by sharing the commit's storage and copying its
# catalogs, with capture's maps carried over, not by restoring the root into empty catalogs.
# (a commit holds nothing of the schema itself, its grants included.  TODO: what making a branch gives
# the branch's schema)
ok(sql('feature', q{SELECT test_vc_branch_at('feature', 'main', 'main-1')}), "a branch of history at main's first commit");
sql('feature', q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner});
sql('feature', qq{SELECT test_ws_root_restore('$first', 'app')});
is(sql('feature', q{SELECT string_agg(name, ',' ORDER BY id) FROM app.customers}), 'ada,grace', 'the branch has the rows');
is(commit_root('feature'), $first, 'and commits the same root');

# 4. Write on both branches, and commit both
sql('main', q{SET ROLE app_owner; SET search_path = app;
	UPDATE customers SET name = 'ada lovelace' WHERE id = 1;
	INSERT INTO notes VALUES ('on main');
	UPDATE documents SET id = id WHERE id = 1});
sql('feature', q{SET ROLE app_owner; SET search_path = app;
	INSERT INTO customers VALUES (3, 'katherine');
	INSERT INTO orders VALUES (3, 3, 120.00);
	DELETE FROM notes WHERE ctid = (SELECT min(ctid) FROM notes)});
my $on_main = commit_root('main');
my $on_feature = commit_root('feature');
is($on_main, read_root('main'), 'a commit on main, of what its tables hold');
is($on_feature, read_root('feature'), 'and on the branch');
isnt($on_main, $on_feature, 'and they differ');

# 5. See what changed.  TODO: dolt_diff().
unlike(sql('main', qq{SELECT test_ws_diff('$first', '$first')}), qr/changed|\+[1-9]|-[1-9]|~[1-9]/, 'a commit diffed against itself: nothing');
my $diff = sql('main', qq{SELECT test_ws_diff('$first', '$on_main')});
like($diff, qr/^rows customers \+0 -0 ~1$/m, 'on main: one customer changed');
like($diff, qr/^rows notes \+1 -0 ~0$/m, 'and one note added');
like($diff, qr/^data orders same$/m, 'and orders the same');
like($diff, qr/^data documents same$/m, 'and a document written to itself the same');
my $feature_diff = sql('feature', qq{SELECT test_ws_diff('$first', '$on_feature')});
like($feature_diff, qr/^rows customers \+1 -0 ~0$/m, 'on the branch: a customer added');
like($feature_diff, qr/^rows orders \+1 -0 ~0$/m, 'and an order');
like($feature_diff, qr/^rows notes \+0 -1 ~0$/m, 'and a note taken away');

# 6. Merge the branch into main, and 7. have the merge as a schema with what both did.  The merge's root
# is held by its transaction alone until it's committed, so it's restored in the same one.  TODO:
# dolt_merge(), which commits it on main with both parents, and finds the base itself.
my $merging = $node->safe_psql('postgres', qq{$on{merged} SELECT test_ws_use_history_store() IS NULL AS on_history \\gset
	BEGIN;
	CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner;
	SELECT test_ws_merge('$first', '$on_main', '$on_feature', 'app') AS merge \\gset
	SELECT split_part(:'merge', E'\\n', 1) AS merged_root \\gset
	SELECT test_ws_root_restore(:'merged_root', 'app');
	COMMIT;
	\\echo :merge});
my ($merged_root, @conflicts) = grep { $_ ne '' } split /\n/, $merging;
like($merged_root, qr/^[0-9a-f]{40}$/, 'a merge');
is(scalar(@conflicts), 0, 'with no conflicts');
is(sql('merged', q{SELECT string_agg(id || ':' || name, ',' ORDER BY id) FROM app.customers}),
	'1:ada lovelace,2:grace,3:katherine', "customers: main's change and the branch's row");
is(sql('merged', q{SELECT string_agg(id || ':' || customer || ':' || total, ',' ORDER BY id) FROM app.orders}),
	'1:1:50.00,2:2:250.00,3:3:120.00', "orders: the branch's added to what was there");
is(sql('merged', q{SELECT string_agg(id || ':' || total, ',' ORDER BY id) FROM app.big_orders}),
	'2:250.00,3:120.00', 'a view over them');
is(sql('merged', q{SELECT conname FROM pg_constraint WHERE conrelid = 'app.orders'::regclass AND contype = 'f'}),
	'orders_customer_fkey', 'their foreign key');
is(sql('merged', q{SELECT last_value || ':' || is_called FROM app.order_ids}), '100:true', 'the sequence where it was');
is(sql('merged', q{SELECT string_agg(body, ',' ORDER BY body) FROM app.notes}), 'first,on main',
	"a table with no key: main's row added, the branch's one of two taken away");
is(sql('merged', q{SELECT length(body) FROM app.documents WHERE id = 1}), '34000', 'a value kept out of line');
is(commit_root('merged'), $merged_root, "a commit of the merged schema holds the merge's root");

# 8. History: each branch's commits, newest first.  TODO: the merge as a commit on main, with both
# parents, rather than a branch of its own.
is(sql('main', q{SELECT test_vc_log('main')}), 'main-2 main-1 init', "main's");
is(sql('feature', q{SELECT test_vc_log('feature')}), 'feature-2 feature-1 main-1 init', "the branch's, from main's first");

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal/m, 'nothing crashed');
$node->stop;
done_testing();
