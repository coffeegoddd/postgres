-- History pushed to the history store's tier as bundles: a push copies what
-- its roots reach that the tier has not into the tier, its rows of
-- pg_history.bundles hold the roots there, and the store lets go of the
-- caller's roots and frees what nothing else holds (what other history
-- holds stays); a read of what was pushed is an error, until fetched back;
-- a release frees in the tier what nothing else holds.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET chunk_free_in_background = off;
SELECT test_chunk_format_refs(true);
CREATE FUNCTION pg_temp.h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
-- (two leaves of their own and one shared; two parents, a root each)
CREATE FUNCTION pg_temp.p1() RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT '\x5202'::bytea || pg_temp.h('\x0a1101') || pg_temp.h('\x0a11ff')$$;
CREATE FUNCTION pg_temp.p2() RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT '\x5202'::bytea || pg_temp.h('\x0a1102') || pg_temp.h('\x0a11ff')$$;
SELECT count(*) FROM unnest(ARRAY['\x0a1101', '\x0a1102', '\x0a11ff']::bytea[]) d,
	LATERAL test_cs_put('pages', d);
SELECT (test_cs_put('pages', pg_temp.p1())).stored AS p1, (test_cs_put('pages', pg_temp.p2())).stored AS p2;
SELECT test_cs_ref_root('pages', pg_temp.h(pg_temp.p1()));
SELECT test_cs_ref_root('pages', pg_temp.h(pg_temp.p2()));

-- the push: three copied (the parent, its leaf, the shared leaf)
SELECT pg_history_push('b1', ARRAY[pg_temp.h(pg_temp.p1())]) AS copied;
SELECT name, roots = ARRAY[pg_temp.h(pg_temp.p1())] AS its_roots FROM pg_history.bundles;
SELECT (test_cs_counts('tier', pg_temp.h(pg_temp.p1()))).roots AS tier_roots;
SELECT test_cs_has('tier', pg_temp.h(pg_temp.p1())) AS p1,
	test_cs_has('tier', pg_temp.h('\x0a1101')) AS leaf1,
	test_cs_has('tier', pg_temp.h('\x0a11ff')) AS shared,
	test_cs_has('tier', pg_temp.h(pg_temp.p2())) AS p2_not;
-- locally: what it alone held let go of; the other parent's kept
SELECT test_cs_has('pages', pg_temp.h(pg_temp.p1())) AS p1,
	test_cs_has('pages', pg_temp.h('\x0a1101')) AS leaf1,
	test_cs_has('pages', pg_temp.h('\x0a11ff')) AS shared,
	test_cs_has('pages', pg_temp.h(pg_temp.p2())) AS p2,
	test_cs_has('pages', pg_temp.h('\x0a1102')) AS leaf2;
SELECT ok FROM test_cs_check('pages');
SELECT ok FROM test_cs_check('tier');

-- a read of what was pushed: an error; so is a root taken of it
SELECT test_cs_get('pages', pg_temp.h(pg_temp.p1()));
SELECT test_cs_get_many('pages', ARRAY[pg_temp.h(pg_temp.p2()), pg_temp.h('\x0a1101')]);
SELECT test_cs_ref_root('pages', pg_temp.h(pg_temp.p1()));
-- (what is shared, and was kept, reads)
SELECT test_cs_get('pages', pg_temp.h('\x0a11ff'));

-- a second bundle sharing what the first has: only what the tier has not
-- copied (the parent and its leaf; the shared leaf is the tier's), and the
-- shared leaf let go of here (the push's holding it in the tier holds
-- nothing here), nothing left unheld
SELECT pg_history_push('b2', ARRAY[pg_temp.h(pg_temp.p2())]) AS copied;
SELECT test_cs_has('pages', pg_temp.h('\x0a11ff')) AS shared_kept;
SELECT ok, unheld FROM test_cs_check('pages');

-- refused: a bundle there already, a root given twice, none, a root the
-- caller does not hold, one the store has not
SELECT pg_history_push('b1', ARRAY[pg_temp.h('\x0a11ff')]);
SELECT (test_cs_put('pages', '\x0a1199')).stored;
SELECT pg_history_push('b3', ARRAY[pg_temp.h('\x0a1199'), pg_temp.h('\x0a1199')]);
SELECT pg_history_push('b3', ARRAY[]::bytea[]);
SELECT pg_history_push('b3', ARRAY[pg_temp.h('\x0a1199')]);
SELECT pg_history_push('b3', ARRAY[pg_temp.h('\x0a1198')]);
-- (a root of a pointer's alone is the pointer's)
INSERT INTO pg_history.pointers VALUES ('main', pg_temp.h('\x0a1199'));
SELECT pg_history_push('b3', ARRAY[pg_temp.h('\x0a1199')]);
DELETE FROM pg_history.pointers WHERE name = 'main';
-- (a bundle's row of a root twice: refused, its roots each once)
INSERT INTO pg_history.bundles VALUES ('dupx', ARRAY[pg_temp.h('\x0a11ff'), pg_temp.h('\x0a11ff')]);
-- the bundles' rows are not changed, nor truncated (the table a system
-- catalog: by one allowed to change those)
UPDATE pg_history.bundles SET name = 'b9' WHERE name = 'b1';
SET allow_system_table_mods = on;
TRUNCATE pg_history.bundles;
RESET allow_system_table_mods;

-- a push aborted: nothing pushed, nothing let go of
SELECT test_cs_ref_root('pages', pg_temp.h('\x0a1199'));
-- (a transaction pushes one bundle)
BEGIN;
SELECT pg_history_push('b4', ARRAY[pg_temp.h('\x0a1199')]) AS copied;
SELECT pg_history_push('b5', ARRAY[pg_temp.h('\x0a1199')]);
ROLLBACK;
BEGIN;
SELECT pg_history_push('b3', ARRAY[pg_temp.h('\x0a1199')]) AS copied;
ROLLBACK;
SELECT test_cs_has('pages', pg_temp.h('\x0a1199')) AS kept,
	(test_cs_counts('pages', pg_temp.h('\x0a1199'))).roots AS roots,
	test_cs_has('tier', pg_temp.h('\x0a1199')) AS not_in_tier,
	(SELECT count(*) FROM pg_history.bundles WHERE name = 'b3') AS no_rows;

-- pointers made and dropped many at once, in one statement each (more
-- distinct chunks than the shared lock table holds locks for: pointers
-- take no lock a chunk)
SELECT count(*) FROM generate_series(1, 20000) g,
	LATERAL test_cs_put('pages', int4send(g));
INSERT INTO pg_history.pointers SELECT 'many' || g, pg_temp.h(int4send(g)) FROM generate_series(1, 20000) g;
DELETE FROM pg_history.pointers WHERE name LIKE 'many%';
SELECT test_cs_free('pages', ARRAY(SELECT pg_temp.h(int4send(g)) FROM generate_series(1, 20000) g)) AS freed;

-- releases: the tier frees what nothing else holds (the shared leaf, held
-- by the other bundle's parent, at the second)
SELECT pg_history_release('b1') AS freed;
SELECT test_cs_has('tier', pg_temp.h('\x0a11ff')) AS shared_kept;
SELECT pg_history_release('b2') AS freed;
SELECT count(*) AS rows FROM pg_history.bundles;
SELECT pg_history_release('b2');
-- and a chunk released is no longer pushed: a read finds nothing
SELECT test_cs_get('pages', pg_temp.h(pg_temp.p1())) IS NULL AS absent;
SELECT ok FROM test_cs_check('tier');

SELECT test_cs_unref_root('pages', pg_temp.h('\x0a1199'));
SELECT test_cs_free('pages', ARRAY[pg_temp.h('\x0a1199')]);
SELECT test_chunk_format_refs(false);
