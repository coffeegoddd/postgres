-- A branch's copy of a table shares its pages, and gets copies of its
-- visibility map and free space map: an all-visible, frozen source's
-- index-only scans on the branch don't visit the heap, as on the source,
-- and VACUUM there finds every page all-visible.
CREATE DATABASE regress_branch_maps;
-- (a copy of the source's storage, not of pages kept as of the commit:
-- VACUUM (FREEZE) after it copies the kept pages it freezes, and a copy
-- of the kept ones has no map true of them; first_use_kept tests those)
ALTER DATABASE regress_branch_maps SET debug_prolly_page_roots = off;
\c regress_branch_maps
CREATE SCHEMA bm VERSIONED;
CREATE TABLE bm.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
INSERT INTO bm.t SELECT g, 'v' || g FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'loaded')::text) AS committed;
VACUUM (FREEZE) bm.t;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
CREATE FUNCTION heap_fetches() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	line text;
BEGIN
	FOR line IN EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
		SELECT count(id) FROM bm.t WHERE id > 0
	LOOP
		IF line LIKE '%Heap Fetches%' THEN
			RETURN trim(line);
		END IF;
	END LOOP;
	RETURN NULL;
END $$;
SELECT heap_fetches() AS on_main;
SELECT dolt_checkout('-b', 'other');
SELECT heap_fetches() AS on_the_branch;
SELECT relallvisible > 0 AS claims_all_visible FROM pg_class WHERE oid = 'bm.t'::regclass;
-- (changed on the branch: its own bits cleared, as any change does)
UPDATE bm.t SET v = 'x' WHERE id <= 100;
SELECT heap_fetches() <> 'Heap Fetches: 0' AS visits_changed_pages;
SELECT count(*) FROM bm.t WHERE v = 'x';
SELECT dolt_checkout('main');
SELECT heap_fetches() AS on_main_after;
SELECT count(*) FROM bm.t WHERE v = 'x';
\c contrib_regression
DROP DATABASE regress_branch_maps;
