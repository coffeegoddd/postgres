# Copyright (c) 2026, PostgreSQL Global Development Group

# PostGIS through a network remote, between two servers: a clone into an
# empty database installs PostGIS in the versioned public it had, and
# every table's values come back byte-identical (EWKB, raster WKB), its
# GiST index agreeing with a sequential scan; a change pushed back, and
# one fetched and pulled, arrive the same.  A clone into a database whose
# PostGIS is in another schema is refused.  Needs PostGIS in the
# installation (the Makefile's check-postgis installs it from
# POSTGIS_BUILD).

use strict;
use warnings FATAL => 'all';
use Cwd qw(abs_path);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $origin = PostgreSQL::Test::Cluster->new('gis_origin');
$origin->init;
$origin->append_conf('postgresql.conf', 'autovacuum = off');
$origin->start;
my $local = PostgreSQL::Test::Cluster->new('gis_local');
$local->init;
$local->append_conf('postgresql.conf', 'autovacuum = off');
$local->start;

my $url = 'postgresql:///gis?host=' . $origin->host . '&port=' . $origin->port;

# The digests a database without the corpus's checks can compute too.
my $digests = q{
	SELECT string_agg(t || '=' || d, ',' ORDER BY t) FROM (
		SELECT 'geoms' AS t, md5(string_agg(id || ':' || coalesce(encode(ST_AsEWKB(g), 'hex'), 'null'), ',' ORDER BY id)) AS d FROM gis_geoms
		UNION ALL SELECT 'geogs', md5(string_agg(id || ':' || coalesce(g::text, 'null'), ',' ORDER BY id)) FROM gis_geogs
		UNION ALL SELECT 'idx', md5(string_agg(id || ':' || coalesce(encode(ST_AsEWKB(g), 'hex'), 'null'), ',' ORDER BY id)) FROM gis_idx
		UNION ALL SELECT 'rasters', md5(string_agg(id || ':' || coalesce(encode(ST_AsBinary(r), 'hex'), 'null'), ',' ORDER BY id)) FROM gis_rasters) s};
my $probe = q{SELECT md5(string_agg(id::text, ',' ORDER BY id)) FROM gis_idx
	WHERE g && ST_MakeEnvelope(0, 0, 12, 9)};
sub scans_agree
{
	my ($node, $db) = @_;
	my $plan = $node->safe_psql($db, "SET enable_seqscan = off; SET enable_bitmapscan = off; EXPLAIN (COSTS OFF) $probe");
	my $index = $node->safe_psql($db, "SET enable_seqscan = off; SET enable_bitmapscan = off; $probe");
	my $seq = $node->safe_psql($db, "SET enable_indexscan = off; SET enable_bitmapscan = off; $probe");
	return $plan =~ /Index Scan using gis_idx_g_idx/ && $index eq $seq;
}

$origin->safe_psql('postgres', 'CREATE DATABASE gis');
$origin->safe_psql('gis', 'DROP SCHEMA public; CREATE SCHEMA public VERSIONED');
$origin->safe_psql('gis', slurp_file(abs_path('sql/gis_corpus.sql')));
$origin->safe_psql('gis', q{
	CREATE INDEX ON gis_idx USING gist (g);
	SELECT dolt_commit('-Am', 'gis corpus');
});
my $src = $origin->safe_psql('gis', $digests);

$local->safe_psql('postgres', 'CREATE DATABASE c');
$local->safe_psql('c', qq{SELECT dolt_clone('$url')});
is($local->safe_psql('c', q{SELECT nspversioned FROM pg_namespace WHERE nspname = 'public'}),
	't', 'the clone made public versioned');
is($local->safe_psql('c', q{SELECT string_agg(extname || ' in ' || extnamespace::regnamespace, ',' ORDER BY extname)
	FROM pg_extension WHERE extname LIKE 'postgis%'}),
	'postgis in public,postgis_raster in public', 'and installed PostGIS there');
is($local->safe_psql('c', $digests), $src, 'the values came back byte-identical');
is($local->safe_psql('c', q{SELECT bool_and(amname = 'prolly') FROM pg_class c JOIN pg_am a ON a.oid = c.relam
	WHERE relname LIKE 'gis\_%' AND relkind = 'r' AND relnamespace = 'public'::regnamespace}),
	't', 'in prolly tables');
ok(scans_agree($local, 'c'), "the clone's GiST index agrees with a sequential scan");

# A change pushed back to the origin.
$local->safe_psql('c', q{
	UPDATE gis_geoms SET g = ST_Translate(g, 0, 10) WHERE id IN (1, 39);
	UPDATE gis_idx SET g = ST_Translate(g, 1, 1) WHERE id % 5 = 0;
	UPDATE gis_rasters SET r = ST_SetValue(r, 1, 1, 1, 42) WHERE id = 7;
	SELECT dolt_commit('-am', 'pushed change');
	SELECT dolt_push('origin', 'main');
});
is($origin->safe_psql('gis', $digests), $local->safe_psql('c', $digests),
	'a push arrives byte-identical');
ok(scans_agree($origin, 'gis'), "and the origin's index agrees");

# A change fetched, then pulled.
$origin->safe_psql('gis', q{
	UPDATE gis_geoms SET g = ST_Buffer(g, 1, 50) WHERE id = 9;
	DELETE FROM gis_idx WHERE id % 7 = 0;
	INSERT INTO gis_geogs VALUES (1000, 'POINT(179.999 -89.999)');
	SELECT dolt_commit('-am', 'origin change');
});
$local->safe_psql('c', q{SELECT dolt_fetch()});
is($local->safe_psql('c', q{SELECT count(*) FROM gis_geogs WHERE id = 1000}),
	'0', 'fetched, not merged');
$local->safe_psql('c', q{SELECT dolt_pull()});
is($local->safe_psql('c', $digests), $origin->safe_psql('gis', $digests),
	'pulled byte-identical');
ok(scans_agree($local, 'c'), 'and the index agrees after the pull');

# PostGIS in another schema than the commit's.
$local->safe_psql('postgres', 'CREATE DATABASE other');
$local->safe_psql('other', 'CREATE SCHEMA gis; CREATE EXTENSION postgis WITH SCHEMA gis');
my ($rc, $out, $err) = $local->psql('other', qq{SELECT dolt_clone('$url')});
like($err, qr/extension postgis: in schema public, and the database has it in gis/,
	'a clone into another layout is refused');
is($local->safe_psql('other', q{SELECT to_regclass('public.gis_geoms') IS NULL}),
	't', 'and clones nothing');

$local->stop;
$origin->stop;
done_testing();
