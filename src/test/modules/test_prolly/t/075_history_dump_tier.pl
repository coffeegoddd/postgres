
# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_dump dumps the history store's tier with the store: each bundle's
# chunks (read from the tier, one the store has too dumped once), then its
# name and roots; pg_restore puts the chunks in the store and pushes each
# bundle again, to the tier the target has.  So a dump does not depend on
# the tier it was taken from.
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
test_prolly.chunk_format_refs = on
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
		AS $$SELECT 'L'::bytea || decode(repeat(md5('d' || i), 20), 'hex')$$;
	CREATE FUNCTION node(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
			(SELECT string_agg(h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))$$;
	-- a tree of leaves 1-20 (a pointer's), and one of 11-40 (a bundle's)
	CREATE FUNCTION ta() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT node(VARIADIC ARRAY(SELECT leaf(i) FROM generate_series(1, 20) i))';
	CREATE FUNCTION tb() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT node(VARIADIC ARRAY(SELECT leaf(i) FROM generate_series(11, 40) i))';
};
$node->safe_psql('src', q{CREATE EXTENSION test_prolly;} . $fns . q{
	SELECT count(*) FROM generate_series(1, 40) i, LATERAL test_cs_put('pages', leaf(i));
	SELECT (test_cs_put('pages', ta())).stored, (test_cs_put('pages', tb())).stored;
	INSERT INTO pg_history.pointers VALUES ('main', h(ta()));
	SELECT test_cs_ref_root('pages', h(tb()));
});
$node->safe_psql('src', q{
	BEGIN;
	SELECT test_cs_unref_root('pages', h(tb()));
	SELECT pg_history_push('b1', ARRAY[h(tb())]);
	COMMIT;
});

my $dir = PostgreSQL::Test::Utils::tempdir;
$node->command_ok([ 'pg_dump', '--format' => 'custom', '--file' => "$dir/src.dump", $node->connstr('src') ],
	'pg_dump of a database with a tier');
$node->safe_psql('postgres', 'CREATE DATABASE dc TEMPLATE template0');
$node->command_ok([ 'pg_restore', "$dir/src.dump", '--dbname' => $node->connstr('dc') ], 'restored');

is($node->safe_psql('dc', q{SELECT name || ' ' || (roots = ARRAY[h(tb())]) FROM pg_history.bundles}),
	'b1 true', 'the bundle, its roots');
# (the bundle's chunks the restore put here, then pushed, let go of: what
# nothing here holds freed, as the freer would)
$node->safe_psql('dc', q{SELECT test_cs_free_unheld('pages')});
my ($ret, $out, $err) = $node->psql('dc', q{SELECT test_cs_get('pages', h(tb()))});
like($err, qr/was pushed to the history store's tier/, 'its root pushed, in the target\'s tier');
is($node->safe_psql('dc', q{SELECT count(*) FILTER (WHERE test_cs_has('tier', h(leaf(i)))) FROM generate_series(11, 40) i}),
	'30', 'its leaves in the tier');
is($node->safe_psql('dc', q{SELECT test_cs_get('pages', h(ta())) = ta()}), 't', 'the pointer\'s tree read here');
is($node->safe_psql('dc', q{SELECT count(*) FILTER (WHERE test_cs_get('pages', h(leaf(i))) = leaf(i)) FROM generate_series(1, 20) i}),
	'20', 'and its leaves, the shared ones too');
# (the freer off: what the fetch put stays as its transaction ends)
is($node->safe_psql('dc', q{SELECT pg_history_fetch('b1')}), '21',
	'fetched back: the root and the leaves not here');
$node->safe_psql('dc', q{SELECT test_cs_ref_root('pages', h(tb()))});
is($node->safe_psql('dc', q{SELECT test_cs_get('pages', h(tb())) = tb()}), 't',
	'the bundle\'s root as it was');
$node->safe_psql('dc', q{SELECT test_cs_free_unheld('pages')});
is($node->safe_psql('dc', q{SELECT ok FROM test_cs_check('pages')}), 't', 'the store\'s audit clean');
is($node->safe_psql('dc', q{SELECT ok FROM test_cs_check('tier')}), 't', 'and the tier\'s');

$node->stop;
done_testing();
