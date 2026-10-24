# SERIALIZABLE with spatial predicates: two transactions each check that
# no zone intersects the one they add, then add overlapping zones, and one
# of them fails.  GiST takes the predicate locks: zones added far apart,
# whose checks read other index pages, both commit.

setup
{
	CREATE EXTENSION IF NOT EXISTS postgis;
	CREATE TABLE gis_zones (id int PRIMARY KEY, g geometry);
	INSERT INTO gis_zones
	SELECT x * 50 + y + 1, ST_MakeEnvelope(x * 10, y * 10, x * 10 + 1, y * 10 + 1)
	FROM generate_series(0, 49) x, generate_series(0, 49) y;
	CREATE INDEX gis_zones_g ON gis_zones USING gist (g);
	ANALYZE gis_zones;
}

teardown
{
	DROP TABLE gis_zones;
}

session s1
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_check	{ SELECT count(*) FROM gis_zones WHERE ST_Intersects(g, ST_MakeEnvelope(2, 2, 8, 8)); }
step s1_add	{ INSERT INTO gis_zones VALUES (-1, ST_MakeEnvelope(2, 2, 8, 8)); }
step s1_commit	{ COMMIT; }

session s2
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_check	{ SELECT count(*) FROM gis_zones WHERE ST_Intersects(g, ST_MakeEnvelope(4, 4, 9, 9)); }
step s2_add	{ INSERT INTO gis_zones VALUES (-2, ST_MakeEnvelope(4, 4, 9, 9)); }
step s2_check_far	{ SELECT count(*) FROM gis_zones WHERE ST_Intersects(g, ST_MakeEnvelope(482, 482, 488, 488)); }
step s2_add_far	{ INSERT INTO gis_zones VALUES (-2, ST_MakeEnvelope(482, 482, 488, 488)); }
step s2_commit	{ COMMIT; }

session s3
step s3_show	{ SELECT id, ST_AsText(g) FROM gis_zones WHERE id < 0 ORDER BY id; }

# overlapping: one of them fails
permutation s1_begin s2_begin s1_check s2_check s1_add s2_add s1_commit s2_commit s3_show
# far apart: both commit
permutation s1_begin s2_begin s1_check s2_check_far s1_add s2_add_far s1_commit s2_commit s3_show
