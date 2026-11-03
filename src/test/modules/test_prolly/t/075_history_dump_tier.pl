
# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_dump dumps the history store's tier with the store: each bundle's
# chunks, read from the tier (one the store has too given again), then its
# name and roots; pg_restore puts those chunks in the tier the target has,
# not in its store, and pushes each bundle again, which copies none.  So a
# dump does not depend on the tier it was taken from, and a restore puts in
# the store only what the source's store had.  A target with no tier is
# refused, its store left as it was.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tier = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('history_dump_tier');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'test_prolly'
autovacuum = off
chunk_free_in_background = off
history_tier_location = '$tier'
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE src');
my $fns = q{
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION leaf(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT test_engine_chunk('L'::bytea || decode(repeat(md5('d' || i), 20), 'hex'))$$;
	CREATE FUNCTION node(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT test_engine_chunk('R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
			(SELECT string_agg(h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o)))$$;
	CREATE FUNCTION tree(a int, b int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT node(VARIADIC ARRAY(SELECT leaf(i) FROM generate_series(a, b) i))';
	-- (whether the store, or the tier, has each of leaves a to b)
	CREATE FUNCTION has(store text, a int, b int) RETURNS int8 LANGUAGE sql
		AS 'SELECT count(*) FILTER (WHERE test_cs_has(store, h(leaf(i)))) FROM generate_series(a, b) i';
};
# a pointer's tree of leaves 1-20; bundle b1's of 11-40, sharing 11-20 with
# it; b2's of 31-50, sharing 31-40 with b1's
$node->safe_psql('src', q{CREATE EXTENSION test_prolly;} . $fns . q{
	SELECT count(*) FROM generate_series(1, 50) i, LATERAL test_cs_put('pages', leaf(i));
	SELECT (test_cs_put('pages', tree(1, 20))).stored, (test_cs_put('pages', tree(11, 40))).stored,
		(test_cs_put('pages', tree(31, 50))).stored;
	INSERT INTO pg_history.pointers VALUES ('main', h(tree(1, 20)));
	SELECT test_cs_ref_root('pages', h(tree(11, 40))), test_cs_ref_root('pages', h(tree(31, 50)));
});
for my $b ([ 'b1', 11, 40 ], [ 'b2', 31, 50 ])
{
	my ($name, $a, $z) = @$b;
	$node->safe_psql('src', qq{
		BEGIN;
		SELECT test_cs_unref_root('pages', h(tree($a, $z)));
		SELECT pg_history_push('$name', ARRAY[h(tree($a, $z))]);
		COMMIT;
	});
}
is($node->safe_psql('src', q{SELECT has('pages', 1, 50), has('tier', 1, 50)}), '20|40',
	'the source: leaves 1-20 in the store, 11-50 in the tier');

my $dir = PostgreSQL::Test::Utils::tempdir;
$node->command_ok([ 'pg_dump', '--format' => 'custom', '--file' => "$dir/src.dump", $node->connstr('src') ],
	'pg_dump of a database with a tier');
$node->safe_psql('postgres', 'CREATE DATABASE dc TEMPLATE template0');
$node->command_ok([ 'pg_restore', "$dir/src.dump", '--dbname' => $node->connstr('dc') ], 'restored');

is($node->safe_psql('dc', q{SELECT string_agg(name || ':' || (roots = ARRAY[h(tree(11, 40))]
		OR roots = ARRAY[h(tree(31, 50))]), ',' ORDER BY name) FROM pg_history.bundles}),
	'b1:true,b2:true', 'the bundles, their roots');
# (the bundles' chunks put in the tier, not staged in the store: it has
# what the source's had, no more)
is($node->safe_psql('dc', q{SELECT has('pages', 1, 50), has('tier', 1, 50)}), '20|40',
	'restored: leaves 1-20 in the store, 11-50 in the tier, none put in the store and pushed');
my ($ret, $out, $err) = $node->psql('dc', q{SELECT test_cs_get('pages', h(tree(11, 40)))});
like($err, qr/was pushed to the history store's tier/, 'b1\'s root pushed, in the target\'s tier');
is($node->safe_psql('dc', q{SELECT test_cs_get('pages', h(tree(1, 20))) = tree(1, 20)}), 't',
	'the pointer\'s tree read here');
# (the freer off: what a fetch put stays as its transaction ends)
is($node->safe_psql('dc', q{SELECT pg_history_fetch('b1')}), '21',
	'b1 fetched back: its root and the leaves not here');
$node->safe_psql('dc', q{SELECT test_cs_ref_root('pages', h(tree(11, 40)))});
is($node->safe_psql('dc', q{SELECT test_cs_get('pages', h(tree(11, 40))) = tree(11, 40)}), 't',
	'b1\'s root as it was');
is($node->safe_psql('dc', q{SELECT pg_history_fetch('b2')}), '11',
	'b2 fetched back: its root and the leaves not here');
is($node->safe_psql('dc', q{SELECT ok FROM test_cs_check('pages')}), 't', 'the store\'s audit clean');
is($node->safe_psql('dc', q{SELECT ok FROM test_cs_check('tier')}), 't', 'and the tier\'s');

# a dump in a snapshot from before a release has the bundle released
$node->safe_psql('src', q{
	SELECT (test_cs_put('pages', leaf(i))).stored FROM generate_series(51, 55) i;
	SELECT (test_cs_put('pages', tree(51, 55))).stored;
	SELECT test_cs_ref_root('pages', h(tree(51, 55)));
	BEGIN;
	SELECT test_cs_unref_root('pages', h(tree(51, 55)));
	SELECT pg_history_push('b3', ARRAY[h(tree(51, 55))]);
	COMMIT;
});
my $snap = $node->background_psql('src');
my $snapshot = $snap->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_export_snapshot();');
$node->safe_psql('src', q{SELECT pg_history_release('b3')});
$node->safe_psql('src', 'VACUUM pg_history.tier_index');
$node->safe_psql('src', 'VACUUM pg_history.tier_chunks');
$node->command_ok([ 'pg_dump', '--format' => 'custom', '--snapshot' => $snapshot,
	'--file' => "$dir/snap.dump", $node->connstr('src') ], 'pg_dump in the snapshot');
$snap->query_safe('COMMIT');
$snap->quit;
$node->safe_psql('postgres', 'CREATE DATABASE ds TEMPLATE template0');
$node->command_ok([ 'pg_restore', "$dir/snap.dump", '--dbname' => $node->connstr('ds') ], 'restored');
is($node->safe_psql('ds', q{SELECT string_agg(name, ',' ORDER BY name) FROM pg_history.bundles}),
	'b1,b2,b3', 'the bundle released since the snapshot, with the others');
is($node->safe_psql('ds', q{SELECT pg_history_fetch('b3')}), '6', 'and its chunks');

# a target with no tier: refused, its store as it was
my $plain = PostgreSQL::Test::Cluster->new('history_dump_no_tier');
$plain->init;
$plain->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'test_prolly'
autovacuum = off
});
$plain->start;
$plain->safe_psql('postgres', 'CREATE DATABASE dn');
my ($rout, $rerr);
IPC::Run::run([ 'pg_restore', "$dir/src.dump", '--dbname' => $plain->connstr('dn') ],
	'>' => \$rout, '2>' => \$rerr);
like($rerr, qr/this server's history store has no tier/, 'a target with no tier refused');
like($rerr, qr/HINT:  Set history_tier_location, or restore without the history/, 'saying how');
is($plain->safe_psql('dn', q{SELECT count(*) FROM pg_history.pointers}), '0', 'its pointers as they were');
is($plain->safe_psql('dn', q{SELECT count(*) FROM pg_history.bundles}), '0', 'and no bundle');
$plain->stop;

$node->stop;
done_testing();
