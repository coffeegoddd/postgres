# Copyright (c) 2026, PostgreSQL Global Development Group

# A move in place on copy-on-write storage leaves what shares its tables' pages as it was.  Each
# table's storage, and an index's, is shared with a copy, as another branch's of the same commit
# would be; the schema is then moved, writing rows, altering a table, making one again and dropping
# one.  Each copy reads, page for page and row for row, what it did; a table the move didn't write
# still shares every page with its copy, and one it wrote shares only those it didn't.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move_shared');
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

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql(q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner; SET search_path = app;
	CREATE TABLE kept (id integer PRIMARY KEY, v text);
	CREATE TABLE still (id integer PRIMARY KEY, v text);
	CREATE TABLE altered (id integer PRIMARY KEY, v text);
	CREATE TABLE remade (id integer PRIMARY KEY, v integer);
	INSERT INTO kept SELECT g, repeat('k', 200) FROM generate_series(1, 2000) g;
	INSERT INTO still SELECT g, repeat('s', 200) FROM generate_series(1, 2000) g;
	INSERT INTO altered SELECT g, 'a' FROM generate_series(1, 500) g;
	INSERT INTO remade SELECT g, g FROM generate_series(1, 500) g});
my $first = commit_root();
sql(q{SET ROLE app_owner; SET search_path = app;
	UPDATE kept SET v = 'changed' WHERE id % 100 = 0;
	ALTER TABLE altered ADD COLUMN w text;
	ALTER TABLE remade ALTER COLUMN v TYPE bigint;
	CREATE TABLE gone (id integer PRIMARY KEY); INSERT INTO gone SELECT generate_series(1, 300)});
my $second = commit_root();

# Each table's storage, and the kept table's key's, shared with a copy, as of the second.
my @rels = qw(kept still altered remade gone kept_pkey);
my (%src, %copy, %rows, %pages);
for my $rel (@rels)
{
	$src{$rel} = sql(qq{SELECT pg_relation_filenode('app.$rel')});
	$copy{$rel} = sql(qq{SELECT test_bmap_share($src{$rel})});
	$pages{$rel} = sql(qq{SELECT test_bmap_digest($copy{$rel})});
	$rows{$rel} = sql(qq{SELECT test_bmap_rows_digest($copy{$rel})}) unless $rel =~ /_pkey$/;
	is(sql(qq{SELECT count(*) FROM test_bmap_diff($src{$rel}, $copy{$rel})}), '0', "$rel: its copy shares every page");
}

sql(qq{SELECT test_ws_move_prepare('app', '$first')});
my $stats = sql(qq{SELECT test_ws_move('app', '$first')});
like($stats, qr/ made 1 dropped 1 altered 1 /, "the move writes rows, alters, makes again and drops: $stats");
is(sql(q{SELECT test_ws_root_capture('app')}), $first, 'the tables hold the first');
is(sql(q{SELECT test_capture_barrier('app')}), $first, 'and capture has it');

for my $rel (@rels)
{
	is(sql(qq{SELECT test_bmap_digest($copy{$rel})}), $pages{$rel}, "$rel: its copy's pages are as they were");
	is(sql(qq{SELECT test_bmap_rows_digest($copy{$rel})}), $rows{$rel}, "$rel: and its rows")
	  unless $rel =~ /_pkey$/;
}
is(sql(q{SELECT pg_relation_filenode('app.still')}), $src{still}, 'still: the move kept its storage');
is(sql(qq{SELECT count(*) FROM test_bmap_diff($src{still}, $copy{still})}), '0', 'still: and it shares every page yet');
is(sql(q{SELECT pg_relation_filenode('app.kept')}), $src{kept}, 'kept: the move wrote in its storage');
my $differ = sql(qq{SELECT count(*) FROM test_bmap_diff($src{kept}, $copy{kept})});
my $nblocks = sql(q{SELECT pg_relation_size('app.kept') / current_setting('block_size')::int});
ok($differ > 0 && $differ < $nblocks, "kept: it shares the pages it didn't write, $differ of $nblocks differing");
isnt(sql(q{SELECT pg_relation_filenode('app.remade')}), $src{remade}, 'remade: made again, in storage of its own');
is(sql(q{SELECT to_regclass('app.gone')}), '', 'gone: dropped');

done_testing();
