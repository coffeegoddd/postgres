-- Another branch's roots held as the session branch's own: a branch made
-- at main's head moves back by main's roots, holding them for itself; a
-- later move carries those entries to the storage it gives the table, so
-- once main lets go of its own, a move back restores by the branch's.

SELECT current_database() AS home \gset
CREATE DATABASE root_holds;
-- (history older than the window keeps no roots: a window long enough
-- for all of this one's)
ALTER DATABASE root_holds SET prolly.history_window = '1000 years';
\c root_holds
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM pg_stat_prolly
	WHERE name IN ('apply_rows', 'relations_restored') $$;
CREATE FUNCTION since(before jsonb) RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$ SELECT key, value::bigint - (before ->> key)::bigint
	FROM jsonb_each_text(counted()) ORDER BY 1 $$;
CREATE SCHEMA rh VERSIONED;
CREATE TABLE rh.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL);
INSERT INTO rh.t SELECT g, 0, repeat('w', 100) FROM generate_series(1, 5000) g;
CREATE FUNCTION digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM rh.t t $$;
SELECT dolt_commit('-Am', 'c0') IS NOT NULL AS committed;
CREATE TEMP TABLE digests (n int, d text);
DO $$ BEGIN FOR i IN 1..10 LOOP
	UPDATE rh.t SET v = i WHERE id % 50 = i;
	PERFORM dolt_commit('-am', 'c' || i);
	INSERT INTO digests VALUES (i, digest());
END LOOP; END $$;
SELECT dolt_hashof('HEAD') AS head \gset
SELECT commit_hash AS c3 FROM dolt_log WHERE message = 'c3' \gset

-- side, made at the head, moves to c3 and back to the head by main's roots
SELECT dolt_branch('side', :'head');
SELECT dolt_checkout('side');
SELECT dolt_reset('--hard', :'c3');
SELECT digest() = (SELECT d FROM digests WHERE n = 3) AS side_at_c3;
SELECT dolt_reset('--hard', :'head');
SELECT count(*) > 0 AS side_keeps_c3 FROM test_kept_commits('side') k
WHERE k.commit_id = :'c3';

-- main keeps only its newest from now on: its roots of c3 let go of
SELECT dolt_checkout('main');
ALTER DATABASE root_holds SET prolly.history_min_commits = 1;
ALTER DATABASE root_holds SET prolly.history_window = '1 microsecond';
DO $$ BEGIN FOR i IN 11..14 LOOP
	UPDATE rh.t SET v = i WHERE id % 50 = i;
	PERFORM dolt_commit('-am', 'c' || i);
END LOOP; END $$;
SELECT test_retain_roots('main');
ALTER DATABASE root_holds RESET prolly.history_min_commits;
ALTER DATABASE root_holds SET prolly.history_window = '1000 years';
SELECT count(*) AS main_kept FROM test_kept_commits('main');

-- side moves back to c3 again: by its own roots
SELECT dolt_checkout('side');
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'c3');
SELECT pg_stat_force_next_flush();
SELECT name, (value > 0)::int FROM since(:'before') WHERE name = 'relations_restored';
SELECT digest() = (SELECT d FROM digests WHERE n = 3) AS side_at_c3_again;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'side');
SELECT count(*) >= 0 AS drained FROM test_bmap_release_drain();
SELECT recounted FROM prolly_page_sweep();

\c :home
DROP DATABASE root_holds;
