# Copyright (c) 2026, PostgreSQL Global Development Group

# What branches cost as there are more of them, and the limits that come with it.  A branch with no
# working set is its head alone: ten thousand of them leave the catalogs as they were, and a catalog
# scan, a lookup and a checkout cost what they did; one of them checked out is given its working set.
# A working set costs C object ids from the cluster's 32-bit counter, one for each object it copies,
# and its catalog rows are read, and hidden, by every scan of a catalog on another branch, so a scan
# costs more with every object on other branches while a lookup by name or key does not.  A thousand
# branches of one table, each with one row changed, store the pages they changed, not a thousand
# copies of the table.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_scale');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on { return "SET catalog_branch = $_[0]; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n"; }
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', on($branch) . $sql);
}
my $unfiltered = 'SET catalog_branch_filter = off; ';
my $schemas = $unfiltered . 'SELECT count(*) FROM pg_namespace WHERE nspbranch <> 0';
my $classes = $unfiltered . 'SELECT count(*) FROM pg_class';

# What the catalog filter does for a query on main: the questions it's asked, the catalog reads made
# to answer them.
sub cost
{
	my ($query) = @_;
	return (split /\n/, $node->safe_psql('postgres', on($main)
		. "SELECT test_branch_map_reset(); $query; SELECT probes || ' probes, ' || reads || ' reads' FROM test_branch_map_stats()"))[-1];
}
my $scan = 'SELECT count(*) FROM pg_class';
my $lookup = q{SELECT count(*) FROM app.t WHERE id = 5; SELECT 'app.t_v'::regclass};
my $checkout = q{SELECT test_branch_checkout('main'); SELECT count(*) FROM app.t WHERE id = 5};

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, v text); CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT g, 'c1' FROM generate_series(1, 100000) g;
	CREATE VIEW app.vw AS SELECT * FROM app.t; CREATE SEQUENCE app.s});
sql($main, "SELECT test_branch_link('main', $main)");
sql($main, q{SELECT test_branch_commit('main', 'c1')});

# Ten thousand heads: the catalogs as they were, and the filter's work too.
my %before = (schemas => $node->safe_psql('postgres', $schemas), scan => cost($scan), lookup => cost($lookup),
	checkout => cost($checkout));
# (a thousand to a transaction, as each takes a lock on its name until it ends)
$node->safe_psql('postgres', "SELECT test_branch_make('h' || g, 'main') FROM generate_series($_ * 1000 + 1, $_ * 1000 + 1000) g")
  for 0 .. 9;
is($node->safe_psql('postgres', q{SELECT count(*) FROM generate_series(1, 10000) g WHERE test_branch_head('h' || g) IS NOT NULL}),
	'10000', 'ten thousand branches made, each a head');
is($node->safe_psql('postgres', $schemas), $before{schemas}, 'with no catalog rows');
is(cost($scan), $before{scan}, 'a catalog scan costs what it did');
is(cost($lookup), $before{lookup}, 'a lookup by name or key too');
is(cost($checkout), $before{checkout}, 'and a checkout');
my $h = (split /\n/, $node->safe_psql('postgres', q{SELECT test_branch_checkout('h5000'); SELECT test_branch_of('h5000')}))[-1];
ok($h > 0, 'one of them checked out is given its working set');
is(sql($h, q{SELECT count(*) FROM app.t}), '100000', 'at its head');

# A working set costs C object ids, one for each object it copies: the schema, each relation (its
# file number the same id), each type and constraint, the view's rule.
my $c = sql($main, q{SELECT (SELECT count(*) FROM pg_namespace WHERE oid = 'app'::regnamespace)
	+ (SELECT count(*) FROM pg_class c WHERE c.relnamespace = 'app'::regnamespace
		OR c.oid IN (SELECT reltoastrelid FROM pg_class WHERE relnamespace = 'app'::regnamespace)
		OR c.oid IN (SELECT i.indexrelid FROM pg_index i JOIN pg_class t ON t.reltoastrelid = i.indrelid
			WHERE t.relnamespace = 'app'::regnamespace))
	+ (SELECT count(*) FROM pg_type WHERE typnamespace = 'app'::regnamespace)
	+ (SELECT count(*) FROM pg_constraint WHERE connamespace = 'app'::regnamespace)
	+ (SELECT count(*) FROM pg_rewrite r JOIN pg_class c ON c.oid = r.ev_class WHERE c.relnamespace = 'app'::regnamespace)});
# (a large object takes one id from the counter, so two bound what's taken between them)
for my $name (qw(o1 o2))
{
	my $from = $node->safe_psql('postgres', 'SELECT lo_create(0)');
	sql($main, qq{SELECT test_branch_create('$name', 'main')});
	my $to = $node->safe_psql('postgres', 'SELECT lo_create(0)');
	is($to - $from - 1, $c, "a branch given a working set takes C object ids ($name)");
}
cmp_ok($c, '>', 10, '(C counts every object the branch has)');

# Every scan of a catalog reads, and hides, the rows of other branches' objects: it costs more with
# each, while a lookup by name or key, which finds only this branch's, does not.
my $classes_before = $node->safe_psql('postgres', $classes);
my ($probes_before) = cost($scan) =~ /^(\d+)/;
$node->safe_psql('postgres', join('', map { on($main) . "SELECT test_branch_create('m$_', 'main');\n" } 1 .. 10));
my $more = $node->safe_psql('postgres', $classes) - $classes_before;
my ($probes_after) = cost($scan) =~ /^(\d+)/;
cmp_ok($more, '>=', 10 * 6, 'ten more working sets, each with its relations\' rows in pg_class');
cmp_ok($probes_after - $probes_before, '>=', $more, 'a scan of pg_class on another branch asks of each of them');
is(cost($lookup), $before{lookup}, 'a lookup by name or key costs what it did');
is(cost($checkout), $before{checkout}, 'and a checkout');

# A thousand branches, each with one row changed: the pages changed, not a thousand copies.
my $pages = sql($main, q{SELECT sum(pg_relation_size(c.oid, f.fork)) / 8192 FROM pg_class c,
	unnest(ARRAY['main', 'fsm', 'vm']) f(fork) WHERE c.relnamespace = 'app'::regnamespace AND c.relkind IN ('r', 'i')});
$node->safe_psql('postgres', 'SELECT test_bmap_drop_orphans(); SELECT * FROM test_bmap_release_drain()');
my $used = $node->safe_psql('postgres', 'SELECT test_bmap_used_blocks()');
$node->safe_psql('postgres', join('', map { on($main) . "SELECT test_branch_create('w$_', 'main');\n" } 1 .. 1000));
$node->safe_psql('postgres', join('', map { "SELECT test_branch_checkout('w$_'); SET ROLE app_owner;
	UPDATE app.t SET v = 'w$_' WHERE id = $_ * 97; RESET ROLE;\n" } 1 .. 1000));
$node->safe_psql('postgres', 'SELECT test_bmap_drop_orphans(); SELECT * FROM test_bmap_release_drain()');
my $grown = $node->safe_psql('postgres', 'SELECT test_bmap_used_blocks()') - $used;
is($node->safe_psql('postgres', q{SELECT count(*) FROM generate_series(1, 1000) g WHERE test_branch_of('w' || g) IS NOT NULL}),
	'1000', 'a thousand branches, each with a working set');
is(sql($node->safe_psql('postgres', q{SELECT test_branch_of('w500')}), q{SELECT v FROM app.t WHERE id = 48500}), 'w500',
	'each with its row changed');
my $own = 0;
for my $w (1, 250, 500, 750, 1000)
{
	my $branch = $node->safe_psql('postgres', qq{SELECT test_branch_of('w$w')});
	for my $rel (qw(app.t app.t_pkey app.t_v))
	{
		my ($theirs, $mine) = map { sql($_, qq{SELECT pg_relation_filenode('$rel')}) } ($main, $branch);
		$own += $node->safe_psql('postgres', "SELECT count(*) FROM test_bmap_diff($theirs, $mine)");
	}
}
cmp_ok($own / 5, '<=', 4, 'each branch\'s own pages are the few its change wrote');
# (and for each, the map's nodes over them, and its catalog rows)
cmp_ok($grown / 1000, '<', 32, 'a thousand branches store that much each');
cmp_ok($grown, '<', $pages * 1000 / 20, 'not a thousand copies of the table');
diag(sprintf('a thousand branches of %d pages: %d blocks, %.1f a branch', $pages, $grown, $grown / 1000));

done_testing();
