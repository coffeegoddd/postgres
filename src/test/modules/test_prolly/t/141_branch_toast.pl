# Copyright (c) 2026, PostgreSQL Global Development Group

# Values stored out of line, on the pages a branch shares with its parent: as the branch is made, each
# page holding one is made the branch's own and its values named the branch's TOAST relation, so the
# branch reads them, whole, in slices and in parallel, and deletes them, from its own, and the
# parent's are as they were.  A value updated or deleted on the branch still reads on the parent; one
# deleted on the parent and vacuumed away there still reads on the branch, and on a branch made from
# it, even once the middle one's table is dropped; a dropped column's are the branch's own too, and a
# materialized view's.  Maintenance run from the parent's session over
# every table (ANALYZE, VACUUM FULL), and a shared trigger function running as its definer, read the
# branch's own.  Pages holding none stay shared.  Every heap is right, its values checked too, capture
# has each branch's rows, a standby reads them, and all of it outlasts a crash, redo of a branch made
# since the last checkpoint too.

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
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('branch_toast_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on { return "SET catalog_branch = $_[0]; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n"; }
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', on($branch) . $sql);
}

# (each value its own: one stored out of line whole, one compressed, still too big to keep in line)
sub big { my ($id, $tag) = @_; return "repeat(md5('$tag' || $id), 400)"; }
sub squeezed
{
	my ($id, $tag) = @_;
	return "(SELECT string_agg(md5(i || '$tag' || $id) || repeat('y', 64), '') FROM generate_series(1, 300) i)";
}
sub reads
{
	my ($branch, $id, $tag, $prefix) = @_;
	return sql($branch, ($prefix // '') . "SELECT big = " . big($id, $tag) . " AND substr(big, 5, 20) = substr("
		  . big($id, $tag) . ", 5, 20) AND squeezed = " . squeezed($id, $tag) . " FROM app.t WHERE id = $id");
}
sub heaps
{
	my ($branch) = @_;
	return sql($branch, q{SELECT count(*) FROM (SELECT r.oid FROM pg_class c,
		LATERAL (VALUES (c.oid), (nullif(c.reltoastrelid, 0))) r(oid)
		WHERE c.relnamespace = 'app'::regnamespace AND c.relkind = 'r' AND r.oid IS NOT NULL OFFSET 0) x,
		LATERAL verify_heapam(x.oid, check_toast => true)});
}
sub capture_right { return sql($_[0], q{SELECT test_capture_barrier('app') = test_ws_root_capture('app')}); }

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, n integer, big text, squeezed text);
	ALTER TABLE app.t ALTER COLUMN big SET STORAGE EXTERNAL});
sql($main, "SET ROLE app_owner; INSERT INTO app.t SELECT g, 0, " . big('g', 'm') . ", " . squeezed('g', 'm')
	  . " FROM generate_series(1, 20) g");
sql($main, q{SET ROLE app_owner; INSERT INTO app.t (id, n) SELECT g, 0 FROM generate_series(100, 3000) g});
# (a column dropped, its values stored out of line still on the pages)
sql($main, q{SET ROLE app_owner; ALTER TABLE app.t ADD COLUMN gone text; ALTER TABLE app.t ALTER COLUMN gone SET STORAGE EXTERNAL;
	UPDATE app.t SET gone = repeat(md5('gone' || id), 400) WHERE id = 10; ALTER TABLE app.t DROP COLUMN gone});
sql($main, "SET ROLE app_owner; CREATE MATERIALIZED VIEW app.mv AS SELECT id, squeezed FROM app.t WHERE id <= 6");
sql($main, "SELECT test_branch_link('main', $main)");
sql($main, q{SELECT test_branch_commit('main', 'c1')});
is(sql($main, q{SELECT count(*) FROM app.t WHERE pg_column_compression(squeezed) IS NOT NULL
	AND pg_column_size(squeezed) < octet_length(squeezed) AND pg_column_toast_chunk_id(squeezed) IS NOT NULL}), '20',
	'main has values stored out of line whole, and compressed');

# A branch of it reads them from its own TOAST relation; the pages holding none stay shared.
my $feature = sql($main, q{SELECT test_branch_create('feature', 'main')});
is(reads($feature, 1, 'm') . reads($feature, 20, 'm'), 'tt', 'a branch reads its parent\'s values, whole and in slices');
is(reads($feature, 7, 'm', 'SET debug_parallel_query = on; '), 't', 'and in parallel');
my $src = sql($main, q{SELECT pg_relation_filenode('app.t')});
my $dst = sql($feature, q{SELECT pg_relation_filenode('app.t')});
my $pages = sql($main, q{SELECT pg_relation_size('app.t') / current_setting('block_size')::int});
my $own = sql($main, qq{SELECT count(*) FROM test_bmap_diff($src, $dst)});
ok($own > 0 && $own < $pages, "the pages holding them are the branch's own ($own of $pages), the rest shared");

# A dropped column's values deleted on the branch: the parent's TOAST relation keeps them.
my $chunks = q{SELECT count(*) FROM pg_toast.pg_toast_} . sql($main, q{SELECT 'app.t'::regclass::oid});
my $had = sql($main, $chunks);
sql($feature, q{SET ROLE app_owner; DELETE FROM app.t WHERE id = 10});
sql($feature, q{VACUUM app.t});
is(sql($main, $chunks), $had, 'a row with a dropped column\'s values deleted on the branch leaves the parent\'s');
is(sql($main, q{SELECT count(*) FROM app.t WHERE id = 10}), '1', 'and its row');

# A materialized view's values: the branch reads them, the parent refreshing its own.
is(sql($feature, "SELECT bool_and(squeezed = " . squeezed('id', 'm') . ") FROM app.mv"), 't',
	'a materialized view\'s values stored out of line read on the branch');

# Changed and deleted on the branch: the parent's are as they were.
sql($feature, "SET ROLE app_owner; UPDATE app.t SET big = " . big(1, 'f') . ", squeezed = " . squeezed(1, 'f')
	  . " WHERE id = 1; DELETE FROM app.t WHERE id = 2");
is(reads($feature, 1, 'f'), 't', 'a value changed on the branch reads as changed there');
is(reads($main, 1, 'm') . reads($main, 2, 'm'), 'tt', 'and the parent\'s, changed and deleted on the branch, read as they were');

# Deleted on the parent, and vacuumed away there: the branch's are as they were.
sql($main, q{SET ROLE app_owner; DELETE FROM app.t WHERE id IN (3, 4, 5)});
sql($main, q{VACUUM app.t});
is(reads($feature, 3, 'm') . reads($feature, 4, 'm'), 'tt', 'values deleted and vacuumed away on the parent read on the branch');

sql($main, q{SET ROLE app_owner; REFRESH MATERIALIZED VIEW app.mv});
sql($main, q{VACUUM app.mv});
is(sql($feature, "SELECT count(*) FILTER (WHERE squeezed = " . squeezed('id', 'm') . ") FROM app.mv"), '6',
	'and still once the parent\'s is refreshed and vacuumed');

# At an older commit, its rows written back on it right after: read from its own.
my $old = sql($main, q{SELECT test_branch_create('old', 'main', test_branch_head('main'))});
is(reads($old, 3, 'm') . reads($old, 1, 'm'), 'tt', 'a branch at an older commit reads the rows written back on it');
is(heaps($old), '0', 'its heap right');

# Maintenance over every table, from a session on the parent: the branch's are its own.
$node->safe_psql('postgres', on($main) . q{ANALYZE});
$node->safe_psql('postgres', on($main) . q{VACUUM FULL});
is(reads($feature, 3, 'm') . reads($feature, 1, 'f') . reads($main, 6, 'm'), 'ttt',
	'ANALYZE and VACUUM FULL of every table, run from the parent, leave each reading its own');

# A shared trigger function, running as its definer on no branch, reads the branch's old values.
$node->safe_psql('postgres', q{CREATE TABLE public.seen (big text, squeezed text);
	CREATE FUNCTION public.saw() RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER
		AS $$BEGIN INSERT INTO public.seen VALUES (OLD.big, OLD.squeezed); RETURN NEW; END$$;
	GRANT EXECUTE ON FUNCTION public.saw() TO PUBLIC});
sql($feature, q{CREATE TRIGGER saw AFTER UPDATE ON app.t FOR EACH ROW EXECUTE FUNCTION public.saw();
	SET ROLE app_owner; UPDATE app.t SET n = 1 WHERE id = 5});
is($node->safe_psql('postgres', "SELECT big = " . big(5, 'm') . " AND squeezed = " . squeezed(5, 'm') . " FROM public.seen"), 't',
	'a shared trigger function running as its definer reads the branch\'s value, gone from the parent');
sql($feature, q{DROP TRIGGER saw ON app.t});

# A branch of the branch: its own copy has them all.
my $deeper = sql($feature, q{SELECT test_branch_create('deeper', 'feature', NULL, true)});
is(reads($deeper, 3, 'm') . reads($deeper, 1, 'f'), 'tt', 'a branch of the branch reads them too, its parent\'s and theirs');
sql($deeper, q{SET ROLE app_owner; DELETE FROM app.t WHERE id = 3});
is(reads($feature, 3, 'm'), 't', 'and deletes them from its own alone');

# The middle one's table dropped: the branch made from it still reads.
sql($feature, q{SET ROLE app_owner; DROP TABLE app.t CASCADE});
is(reads($deeper, 4, 'm'), 't', 'a branch\'s table dropped, the branch made from it still reads its values');
sql($feature, q{SET ROLE app_owner; CREATE TABLE app.t (id integer PRIMARY KEY, n integer, big text, squeezed text)});

# Every heap right, its values checked; capture with each.
is(heaps($main) . heaps($deeper), '00', 'every heap right, its values stored out of line too');
is(capture_right($main) . capture_right($deeper), 'tt', 'and capture has each branch\'s rows');

# A standby has them as each branch does.
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', "SET catalog_branch = $deeper; SELECT big = " . big(4, 'm') . " FROM app.t WHERE id = 4"), 't',
	'a standby reads each branch\'s values as it does');

# Made after a checkpoint, then a crash: redo writes the pages made the branch's own.
$node->safe_psql('postgres', 'CHECKPOINT');
my $after = sql($main, q{SELECT test_branch_create('after_checkpoint', 'main')});
$node->stop('immediate');
$node->start;
is(reads($after, 6, 'm') . reads($deeper, 4, 'm') . reads($deeper, 1, 'f') . reads($main, 1, 'm'), 'tttt',
	'after a crash, each reads as it did, one made since the checkpoint too');
is(heaps($after), '0', 'its heap right');
$standby->stop;

done_testing();
