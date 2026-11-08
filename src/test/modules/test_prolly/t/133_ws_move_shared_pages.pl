# Copyright (c) 2026, PostgreSQL Global Development Group

# A move in place on copy-on-write storage leaves what shares its tables' pages as it was.  Each
# relation's storage (tables, a TOAST table and its index, an index, a sequence, a materialized
# view) is shared with a copy, as another branch's of the same commit would be, one through a root
# captured of it; the schema is then moved, writing rows, altering a table in place, making one
# again, dropping one, resetting a sequence and refreshing a view.  With what the move let go of freed
# and written over, and each copy read from storage after a restart, and again after a crash in a
# second move, each reads page for page and row for row what it did; a table the move didn't write
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
	CREATE TABLE kept (id integer PRIMARY KEY, v text, big text);
	ALTER TABLE kept ALTER COLUMN big SET STORAGE EXTERNAL;
	CREATE TABLE still (id integer PRIMARY KEY, v text);
	CREATE TABLE altered (id integer PRIMARY KEY, v text);
	CREATE TABLE remade (id integer PRIMARY KEY, v integer);
	CREATE SEQUENCE s;
	INSERT INTO kept SELECT g, repeat('k', 200), CASE WHEN g <= 20 THEN repeat(md5(g::text), 200) END
		FROM generate_series(1, 2000) g;
	INSERT INTO still SELECT g, repeat('s', 200) FROM generate_series(1, 2000) g;
	INSERT INTO altered SELECT g, 'a' FROM generate_series(1, 500) g;
	INSERT INTO remade SELECT g, g FROM generate_series(1, 500) g;
	SELECT setval('s', 10);
	CREATE MATERIALIZED VIEW seen AS SELECT id, v FROM kept WHERE id % 10 = 0});
my $first = commit_root();
sql(q{SET ROLE app_owner; SET search_path = app;
	UPDATE kept SET v = 'changed' WHERE id % 100 = 0;
	UPDATE kept SET big = repeat(md5((id + 1)::text), 200) WHERE id <= 10;
	ALTER TABLE altered ADD COLUMN w text;
	ALTER TABLE remade ALTER COLUMN v TYPE bigint;
	CREATE TABLE gone (id integer PRIMARY KEY); INSERT INTO gone SELECT generate_series(1, 300);
	SELECT setval('s', 99);
	REFRESH MATERIALIZED VIEW seen});
my $second = commit_root();

# Each relation's storage shared with a copy, as of the second; one of them through a root captured
# of it.
my %rel = map { $_ => "app.$_" } qw(kept still altered remade gone kept_pkey s seen);
$rel{toast} = sql(q{SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 'app.kept'::regclass});
$rel{toast_index} = sql(qq{SELECT indexrelid::regclass FROM pg_index WHERE indrelid = '$rel{toast}'::regclass});
my %heap = map { $_ => 1 } qw(kept still altered remade gone s seen toast);
my (%src, %copy, %rows, %pages);
for my $name (sort keys %rel)
{
	$src{$name} = sql(qq{SELECT pg_relation_filenode('$rel{$name}')});
	$copy{$name} = sql(qq{SELECT test_bmap_share($src{$name})});
	is(sql(qq{SELECT count(*) FROM test_bmap_diff($src{$name}, $copy{$name})}), '0', "$name: its copy shares every page");
}
$copy{still_root} = sql(qq{SELECT test_bmap_share_root(test_bmap_root_capture($src{still}))});
$heap{still_root} = 1;
for my $name (sort keys %copy)
{
	$pages{$name} = sql(qq{SELECT test_bmap_digest($copy{$name})});
	$rows{$name} = sql(qq{SELECT test_bmap_rows_digest($copy{$name})}) if $heap{$name};
}
ok(sql(qq{SELECT count(*) FROM $rel{toast}}) > 0, 'kept has values out of line');

# Each copy read from storage, not from its buffers.
sub copies_as_they_were
{
	my ($when) = @_;
	sql('CHECKPOINT');
	$node->restart;
	for my $name (sort keys %copy)
	{
		is(sql(qq{SELECT test_bmap_digest($copy{$name})}), $pages{$name}, "$when, $name: its copy's pages are as they were");
		is(sql(qq{SELECT test_bmap_rows_digest($copy{$name})}), $rows{$name}, "$when, $name: and its rows")
		  if $heap{$name};
	}
}

sql(qq{SELECT test_ws_move_prepare('app', '$first')});
my $stats = sql(qq{SELECT test_ws_move('app', '$first')});
like($stats, qr/ made 1 dropped 1 altered 1 /, "the move writes rows, alters, makes again and drops: $stats");
is(sql(q{SELECT test_ws_root_capture('app')}), $first, 'the tables hold the first');
is(sql(q{SELECT test_capture_barrier('app')}), $first, 'and capture has it');
is(sql(q{SELECT pg_relation_filenode('app.altered')}), $src{altered}, 'altered: altered in place, in its storage');
is(sql(q{SELECT pg_relation_filenode('app.still')}), $src{still}, 'still: the move kept its storage');
is(sql(qq{SELECT count(*) FROM test_bmap_diff($src{still}, $copy{still})}), '0', 'still: and it shares every page yet');
is(sql(q{SELECT pg_relation_filenode('app.kept')}), $src{kept}, 'kept: the move wrote in its storage');
my $differ = sql(qq{SELECT count(*) FROM test_bmap_diff($src{kept}, $copy{kept})});
ok($differ > 0, "kept: some pages are no longer shared, $differ");
ok(sql(qq{SELECT count(*) < pg_relation_size('app.kept') / current_setting('block_size')::int
	FROM test_bmap_diff($src{kept}, $copy{kept})}) eq 't', 'kept: but those it didn\'t write are');
isnt(sql(q{SELECT pg_relation_filenode('app.remade')}), $src{remade}, 'remade: made again, in storage of its own');
is(sql(q{SELECT to_regclass('app.gone')}), '', 'gone: dropped');
is(sql(q{SELECT last_value FROM app.s}), '10', 's: set to the first\'s');

# What the move let go of freed, and written over by what comes after.
ok(sql(q{SELECT entries FROM test_bmap_release_drain(0)}) > 0, 'what the move let go of is drained');
sql(q{SELECT * FROM test_bmap_sweep()});
sql(q{CREATE TABLE public.filler AS SELECT g, repeat('f', 500) AS v FROM generate_series(1, 20000) g});
copies_as_they_were('after the move');

# A crash in a second move, back to the second, once it commits: redo writes what it wrote.
sql(qq{SELECT test_ws_move_prepare('app', '$second')});
sql(qq{SELECT test_ws_move('app', '$second')});
$node->stop('immediate');
$node->start;
is(sql(q{SELECT test_ws_root_capture('app')}), $second, 'after a crash in a second move, the tables hold the second');
copies_as_they_were('after a crash');

$node->stop;
done_testing();
