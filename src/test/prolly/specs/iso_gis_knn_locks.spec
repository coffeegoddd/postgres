# Row locks on a KNN result (ORDER BY g <-> point LIMIT 1 FOR UPDATE), as
# a dispatcher takes the nearest free vehicle: a second taker waits for the
# first, then re-checks the row it locked; with SKIP LOCKED it takes the
# next nearest at once.

setup
{
	CREATE EXTENSION IF NOT EXISTS postgis;
	CREATE TABLE gis_fleet (id int PRIMARY KEY, g geometry, busy bool DEFAULT false);
	INSERT INTO gis_fleet (id, g)
	SELECT i, ST_MakePoint(i, i) FROM generate_series(1, 20) i;
	CREATE INDEX gis_fleet_g ON gis_fleet USING gist (g);
}

teardown
{
	DROP TABLE gis_fleet;
}

session s1
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s1_begin	{ BEGIN; }
step s1_take	{ SELECT id FROM gis_fleet WHERE NOT busy ORDER BY g <-> ST_MakePoint(0, 0) LIMIT 1 FOR UPDATE; }
step s1_busy	{ UPDATE gis_fleet SET busy = true WHERE id = 1; }
step s1_move	{ UPDATE gis_fleet SET g = ST_MakePoint(100, 100) WHERE id = 1; }
step s1_commit	{ COMMIT; }

session s2
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s2_begin	{ BEGIN; }
step s2_take	{ SELECT id FROM gis_fleet WHERE NOT busy ORDER BY g <-> ST_MakePoint(0, 0) LIMIT 1 FOR UPDATE; }
step s2_skip	{ SELECT id FROM gis_fleet WHERE NOT busy ORDER BY g <-> ST_MakePoint(0, 0) LIMIT 1 FOR UPDATE SKIP LOCKED; }
step s2_commit	{ COMMIT; }

# waits, then re-checks the row: busy now, so it takes the next nearest
permutation s1_begin s2_begin s1_take s2_take s1_busy s1_commit s2_commit
# waits, then finds the row moved: still free, so still returned (the
# order isn't re-checked, only the condition)
permutation s1_begin s2_begin s1_take s2_take s1_move s1_commit s2_commit
# skips the locked nearest and takes the next
permutation s1_begin s2_begin s1_take s2_skip s1_commit s2_commit
