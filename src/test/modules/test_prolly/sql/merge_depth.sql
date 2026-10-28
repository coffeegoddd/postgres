-- A three-way merge's work follows what changed since its base, not how
-- far back the base is.  Two merges of one branch, whose base is 10 and 50
-- commits back, each side changing the same rows: the same table work
-- (rows applied, table nodes read), and the commits read differ by the 40
-- commits between, each read once, to find the base.  The merge's commit
-- records the commits it reaches from its base's count and theirs, the
-- base found as the merge's was: the commits between aren't read again.
SELECT current_database() AS home \gset
CREATE DATABASE regress_merge_depth;
\c regress_merge_depth
CREATE EXTENSION test_prolly;
CREATE FUNCTION counted(name text) RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce((SELECT value FROM pg_stat_prolly
					 WHERE pg_stat_prolly.name = counted.name), 0)
$$;
-- (the count planned, and the log's rows counted: the same)
CREATE FUNCTION counts(ref text) RETURNS text LANGUAGE sql AS $$
	SELECT (SELECT count(*) FROM dolt_log(ref)) || ' ' ||
		(SELECT count(*) FROM (SELECT * FROM dolt_log(ref) OFFSET 0) l)
$$;
CREATE TABLE merges (depth int, commits_read bigint, table_nodes_read bigint,
					 apply_rows bigint);

CREATE SCHEMA md VERSIONED;
CREATE TABLE md.t (id int PRIMARY KEY, rev int);
INSERT INTO md.t SELECT g, 0 FROM generate_series(1, 2000) g;
SELECT length(dolt_commit('-Am', 'rows')::text) > 0 AS committed;
-- every commit changes the same rows
DO $$
BEGIN
	FOR i IN 1..60 LOOP
		UPDATE md.t SET rev = i WHERE id <= 10;
		PERFORM dolt_commit('-am', 'c' || i);
	END LOOP;
END
$$;
SELECT dolt_branch('fixture');

-- a branch at fixture~depth, a commit of other rows on it, and a merge of
-- fixture into it
CREATE FUNCTION merge_at(depth int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM dolt_checkout('main');
	PERFORM dolt_branch('at' || depth, 'fixture~' || depth);
	PERFORM dolt_checkout('at' || depth);
	UPDATE md.t SET rev = -1 WHERE id BETWEEN 1001 AND 1010;
	PERFORM dolt_commit('-am', 'side');
END
$$;
\set measure 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set record 'SELECT pg_stat_force_next_flush(); INSERT INTO merges SELECT :depth, counted(''commits_read''), counted(''nodes_read'') - counted(''refs_nodes_read''), counted(''apply_rows'');'

\set depth 10
SELECT merge_at(:depth);
:measure
SELECT (dolt_merge('fixture', '-m', 'merge'))[3:4] AS merged;
:record
SELECT counts('at10') AS at10_counts, test_commit_commits(dolt_hashof('at10')) AS recorded;

\set depth 50
SELECT merge_at(:depth);
:measure
SELECT (dolt_merge('fixture', '-m', 'merge'))[3:4] AS merged;
:record
SELECT counts('at50') AS at50_counts, test_commit_commits(dolt_hashof('at50')) AS recorded;

-- the same table work at both depths; the commits between read once each
-- (three times each before: the base's walk, then the merge commit's count
-- walking them again, reading each twice)
SELECT d.apply_rows = s.apply_rows AS same_rows_applied,
	   d.table_nodes_read = s.table_nodes_read AS same_table_nodes_read,
	   d.commits_read - s.commits_read <= 40 AS commits_between_read_once
FROM merges s, merges d WHERE s.depth = 10 AND d.depth = 50;

-- each merged branch has its rows and fixture's
SELECT dolt_checkout('at50') IS NOT NULL AS on_at50;
SELECT count(*) FILTER (WHERE rev = 60) AS theirs,
	   count(*) FILTER (WHERE rev = -1) AS ours
FROM md.t;

-- a merge whose parents have two maximal common ancestors (criss-cross)
-- counts its commits by a walk: the counts still agree
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('x1', 'fixture~5');
SELECT dolt_branch('x2', 'fixture~5');
SELECT dolt_checkout('x1') IS NOT NULL AS on_x1;
UPDATE md.t SET rev = -2 WHERE id = 1500;
SELECT length(dolt_commit('-am', 'x1')::text) > 0 AS committed;
SELECT dolt_checkout('x2') IS NOT NULL AS on_x2;
UPDATE md.t SET rev = -3 WHERE id = 1600;
SELECT length(dolt_commit('-am', 'x2')::text) > 0 AS committed;
SELECT dolt_branch('x2a', 'x2');
SELECT (dolt_merge('x1', '--no-ff', '-m', 'x1 into x2'))[3:4] AS merged;
SELECT dolt_checkout('x1') IS NOT NULL AS on_x1;
SELECT (dolt_merge('x2a', '--no-ff', '-m', 'x2 into x1'))[3:4] AS merged;
UPDATE md.t SET rev = -4 WHERE id = 1700;
SELECT length(dolt_commit('-am', 'x1 again')::text) > 0 AS committed;
SELECT dolt_checkout('x2') IS NOT NULL AS on_x2;
SELECT (dolt_merge('x1', '--no-ff', '-m', 'criss-cross'))[3:4] AS merged;
SELECT counts('x2') AS x2_counts, test_commit_commits(dolt_hashof('x2')) AS recorded;

\c :home
DROP DATABASE regress_merge_depth;
