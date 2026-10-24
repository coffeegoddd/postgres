# An exclusion constraint on geometries (EXCLUDE USING gist (g WITH &&)):
# a second transaction adding an overlapping parcel waits for the first,
# then fails if it committed and succeeds if it rolled back; a parcel that
# doesn't overlap doesn't wait.

setup
{
	CREATE EXTENSION IF NOT EXISTS postgis;
	CREATE TABLE gis_parcels (id int PRIMARY KEY, g geometry,
		EXCLUDE USING gist (g WITH &&));
	INSERT INTO gis_parcels VALUES (1, ST_MakeEnvelope(0, 0, 1, 1));
}

teardown
{
	DROP TABLE gis_parcels;
}

session s1
step s1_begin	{ BEGIN; }
step s1_add	{ INSERT INTO gis_parcels VALUES (10, ST_MakeEnvelope(10, 10, 20, 20)); }
step s1_commit	{ COMMIT; }
step s1_rollback	{ ROLLBACK; }

session s2
step s2_begin	{ BEGIN; }
step s2_overlap	{ INSERT INTO gis_parcels VALUES (11, ST_MakeEnvelope(15, 15, 25, 25)); }
step s2_apart	{ INSERT INTO gis_parcels VALUES (12, ST_MakeEnvelope(30, 30, 40, 40)); }
step s2_commit	{ COMMIT; }

session s3
step s3_show	{ SELECT id, ST_AsText(g) FROM gis_parcels ORDER BY id; }

permutation s1_begin s2_begin s1_add s2_overlap s1_commit s2_commit s3_show
permutation s1_begin s2_begin s1_add s2_overlap s1_rollback s2_commit s3_show
permutation s1_begin s2_begin s1_add s2_apart s1_commit s2_commit s3_show
