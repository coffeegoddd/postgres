# Copyright (c) 2026, PostgreSQL Global Development Group

# Durability with PostGIS: after a crash in the middle of a geometry bulk
# load, the committed geometries are there byte for byte (EWKB digests,
# never row_to_json, whose GeoJSON keeps 9 digits), the load's are gone,
# and the GiST index agrees with the table.  Every page replayed is checked
# against the primary's image (wal_consistency_checking).  On prolly, a
# crash in the middle of a commit of large geometries (in its fold, and
# with its chunks reserved but its transaction not committed) leaves the
# head where it was and the working set as it was; the commit then
# succeeds, and both commits read back.  Needs PostGIS in the installation
# (the Makefile's check-postgis installs it from POSTGIS_BUILD).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $am = $ENV{PROLLY_AM} // 'prolly';

my $node = PostgreSQL::Test::Cluster->new('gis_crash');
$node->init;
$node->append_conf('postgresql.conf', qq(
default_table_access_method = '$am'
wal_consistency_checking = 'all'
));
$node->start;
setup_target($node, 'postgres', $am);

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Per-table digests over EWKB, and which ids a GiST scan finds (every
# geometry the probe box meets) against a sequential scan's.
my $digest = q{
	SELECT count(*) || ' ' || md5(string_agg(id || ':' || encode(ST_AsEWKB(g), 'hex'), ',' ORDER BY id))
	FROM gis_load};
my $probe = q{SELECT md5(string_agg(id::text, ',' ORDER BY id)) FROM gis_load
	WHERE g && ST_MakeEnvelope(100, 100, 3000, 3000)};
sub scans_agree
{
	my $index = sql("SET enable_seqscan = off; SET enable_bitmapscan = off; $probe");
	my $seq = sql("SET enable_indexscan = off; SET enable_bitmapscan = off; $probe");
	return $index eq $seq;
}

sql(q{
	CREATE EXTENSION postgis WITH SCHEMA public;
	CREATE TABLE gis_load (id int PRIMARY KEY, g geometry);
	CREATE INDEX gis_load_g ON gis_load USING gist (g);
	-- points, lines, and polygons large enough to go out of line
	INSERT INTO gis_load SELECT i, ST_MakePoint(i, i * 0.5) FROM generate_series(1, 4000) i;
	INSERT INTO gis_load
	SELECT 4000 + i, ST_MakeLine(ST_MakePoint(i, 0), ST_MakePoint(i * 2, i * 3))
	FROM generate_series(1, 1000) i;
	INSERT INTO gis_load
	SELECT 5000 + i, ST_Buffer(ST_MakePoint(i * 100, i * 100), 40, 2000)
	FROM generate_series(1, 20) i;
	CHECKPOINT;
	UPDATE gis_load SET g = ST_Translate(g, 1, 1) WHERE id % 10 = 0;
	DELETE FROM gis_load WHERE id % 7 = 0;
	UPDATE gis_load SET g = ST_Buffer(ST_Centroid(g), 50, 2500) WHERE id BETWEEN 5001 AND 5005;
});
cmp_ok(sql(q{SELECT count(*) FROM gis_load WHERE pg_column_size(g) > 8000}),
	'>=', 15, 'large polygons stored out of line');

# A bulk load in flight at the crash.
my $load = $node->background_psql('postgres');
$load->query_safe(q{BEGIN;
	INSERT INTO gis_load
	SELECT 10000 + i, ST_Buffer(ST_MakePoint(i, -i), 5, 500) FROM generate_series(1, 300) i;});

my $expect = sql($digest);
$node->stop('immediate');
$load->quit;
$node->start;

is(sql($digest), $expect, 'committed geometries survive a crash byte for byte');
is(sql('SELECT count(*) FROM gis_load WHERE id > 10000'), '0',
	'the uncommitted load is gone');
ok(scans_agree(), 'the GiST index agrees with the table after recovery');

# Writes after recovery, and a second crash.
sql(q{INSERT INTO gis_load VALUES (-1, ST_Buffer('POINT(0 0)'::geometry, 9, 3000))});
$expect = sql($digest);
$node->stop('immediate');
$node->start;
is(sql($digest), $expect, 'geometries written after recovery survive the next crash');

if ($am eq 'prolly' && $ENV{enable_injection_points} eq 'yes')
{
	sql('CREATE EXTENSION injection_points WITH SCHEMA extensions');
	my $c1 = sql(q{SELECT (dolt_commit('-Am', 'loaded'))[1]});
	my $at_c1 = sql($digest);

	# A commit of large geometries, stopped by a crash as its fold reads
	# the branch's state, and once its chunks are reserved in the journal
	# but its transaction hasn't committed.
	sql(q{UPDATE gis_load SET g = ST_Translate(g, 0.5, 0) WHERE id > 5000 OR id % 100 = 1});
	my $edited = sql($digest);
	foreach my $point ('prolly-fold-stamp', 'prolly-append-reserved')
	{
		my $committer = $node->background_psql('postgres');
		$committer->query_safe(qq{SELECT injection_points_set_local();
			SELECT injection_points_attach('$point', 'wait')});
		$committer->query_until(qr/started/, q{
			\echo started
			SELECT dolt_commit('-am', 'large edit');
		});
		$node->poll_query_until('postgres', qq{SELECT count(*) > 0 FROM pg_stat_activity
			WHERE wait_event = '$point'}) or die "the commit did not stop at $point";
		$node->stop('immediate');
		$committer->quit;
		$node->start;

		is(sql(q{SELECT dolt_hashof('HEAD')}), $c1,
			"a crash at $point leaves the head where it was");
		is(sql($digest), $edited, 'and the working set as it was');
	}
	my $c2 = sql(q{SELECT (dolt_commit('-am', 'large edit'))[1]});
	isnt($c2, $c1, 'the commit then succeeds');
	sql(qq{SELECT dolt_reset('--hard', '$c1')});
	is(sql($digest), $at_c1, 'the commit before the crash reads back');
	ok(scans_agree(), 'the GiST index agrees at it');
	sql(qq{SELECT dolt_reset('--hard', '$c2')});
	is(sql($digest), $edited, 'the commit after it reads back');
	ok(scans_agree(), 'the GiST index agrees there too');
}

unlike(slurp_file($node->logfile), qr/inconsistent page found/,
	'no inconsistent pages in replay');
$node->stop;
done_testing();
