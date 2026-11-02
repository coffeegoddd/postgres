-- History pushed to the history store's tier as bundles: a push copies what
-- its roots reach that the tier has not into the tier, its row of
-- pg_history.bundles holds the roots there, and the store frees what
-- nothing holds any more (the caller letting go of its roots first, in the
-- push's transaction; what anything else holds stays); a read of what was
-- pushed is an error, until fetched back; a release frees in the tier what
-- nothing else holds; and a fetch copies a bundle back.
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

-- the push: three copied (the parent, its leaf, the shared leaf), the
-- caller's root let go of first
BEGIN;
SELECT test_cs_unref_root('pages', pg_temp.h(pg_temp.p1()));
SELECT pg_history_push('b1', ARRAY[pg_temp.h(pg_temp.p1())]) AS copied;
COMMIT;
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
BEGIN;
SELECT test_cs_unref_root('pages', pg_temp.h(pg_temp.p2()));
SELECT pg_history_push('b2', ARRAY[pg_temp.h(pg_temp.p2())]) AS copied;
COMMIT;
SELECT test_cs_has('pages', pg_temp.h('\x0a11ff')) AS shared_kept;
SELECT ok, unheld FROM test_cs_check('pages');

-- refused: a bundle there already, a root given twice, none, one the store
-- has not
SELECT pg_history_push('b1', ARRAY[pg_temp.h('\x0a11ff')]);
SELECT (test_cs_put('pages', '\x0a1199')).stored;
SELECT pg_history_push('b3', ARRAY[pg_temp.h('\x0a1199'), pg_temp.h('\x0a1199')]);
SELECT pg_history_push('b3', ARRAY[]::bytea[]);
SELECT pg_history_push('b3', ARRAY[pg_temp.h('\x0a1198')]);
-- a chunk something still holds (a pointer, here): pushed, and kept here,
-- read here still; the pointer as it was
INSERT INTO pg_history.pointers VALUES ('main', pg_temp.h('\x0a1199'));
SELECT pg_history_push('b6', ARRAY[pg_temp.h('\x0a1199')]) AS copied;
SELECT test_cs_get('pages', pg_temp.h('\x0a1199')) AS read_here,
	(test_cs_counts('pages', pg_temp.h('\x0a1199'))).roots AS roots,
	test_cs_has('tier', pg_temp.h('\x0a1199')) AS in_tier;
DELETE FROM pg_history.pointers WHERE name = 'main';
SELECT pg_history_release('b6') AS freed;
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
BEGIN;
SELECT test_cs_unref_root('pages', pg_temp.h('\x0a1199'));
SELECT pg_history_push('b3', ARRAY[pg_temp.h('\x0a1199')]) AS copied;
ROLLBACK;
SELECT test_cs_has('pages', pg_temp.h('\x0a1199')) AS kept,
	(test_cs_counts('pages', pg_temp.h('\x0a1199'))).roots AS roots,
	test_cs_has('tier', pg_temp.h('\x0a1199')) AS not_in_tier,
	(SELECT count(*) FROM pg_history.bundles WHERE name = 'b3') AS no_rows;

-- pointers made and dropped many at once, in one statement each (more
-- distinct chunks than the shared lock table holds locks for)
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
-- a fetch: what a bundle's roots reach copied back from the tier, the
-- tier keeping its copy; the caller takes its roots in the fetch's
-- transaction; a fetch again copies nothing; a release then leaves the
-- copy here
CREATE FUNCTION pg_temp.p7() RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT '\x5201'::bytea || pg_temp.h('\x0a1701')$$;
SELECT (test_cs_put('pages', '\x0a1701')).stored, (test_cs_put('pages', pg_temp.p7())).stored;
SELECT test_cs_ref_root('pages', pg_temp.h(pg_temp.p7()));
BEGIN;
SELECT test_cs_unref_root('pages', pg_temp.h(pg_temp.p7()));
SELECT pg_history_push('b7', ARRAY[pg_temp.h(pg_temp.p7())]) AS copied;
COMMIT;
SELECT test_cs_get('pages', pg_temp.h(pg_temp.p7()));
-- (a fetch aborted: still pushed)
BEGIN;
SELECT pg_history_fetch('b7') AS copied;
ROLLBACK;
SELECT test_cs_get('pages', pg_temp.h(pg_temp.p7()));
BEGIN;
SELECT pg_history_fetch('b7') AS copied;
SELECT test_cs_ref_root('pages', pg_temp.h(pg_temp.p7()));
COMMIT;
SELECT test_cs_get('pages', pg_temp.h(pg_temp.p7())) = pg_temp.p7() AS read_here,
	test_cs_get('pages', pg_temp.h('\x0a1701')) AS leaf_here,
	test_cs_has('tier', pg_temp.h(pg_temp.p7())) AS tier_keeps;
SELECT ok, unheld FROM test_cs_check('pages');
SELECT pg_history_fetch('b7') AS copied_again;
SELECT pg_history_release('b7') AS freed;
SELECT test_cs_get('pages', pg_temp.h(pg_temp.p7())) = pg_temp.p7() AS read_still;
SELECT pg_history_fetch('b7');
-- a fetch stops at what the store has (the leaf p7 holds here), and puts
-- again what was freed here but not taken out yet
CREATE FUNCTION pg_temp.p8() RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT '\x5202'::bytea || pg_temp.h('\x0a1701') || pg_temp.h('\x0a1801')$$;
SELECT (test_cs_put('pages', '\x0a1801')).stored, (test_cs_put('pages', pg_temp.p8())).stored;
SELECT pg_history_push('b8', ARRAY[pg_temp.h(pg_temp.p8())]) AS copied;
SELECT pg_history_fetch('b8') AS copied_but_the_leaf_here;
SELECT test_cs_free('pages', ARRAY[pg_temp.h(pg_temp.p8())]) AS freed_here;
SELECT pg_history_fetch('b8') AS put_again;
SELECT test_cs_get('pages', pg_temp.h(pg_temp.p8())) = pg_temp.p8() AS read_here;
-- (a fetch in a read-only transaction, and by one not a superuser, granted
-- it: refused)
BEGIN READ ONLY;
SELECT pg_history_fetch('b8');
ROLLBACK;
CREATE ROLE regress_history_fetcher;
GRANT EXECUTE ON FUNCTION pg_history_fetch(text) TO regress_history_fetcher;
SET ROLE regress_history_fetcher;
SELECT pg_history_fetch('b8');
RESET ROLE;
DROP OWNED BY regress_history_fetcher;
DROP ROLE regress_history_fetcher;
SELECT pg_history_release('b8') AS freed;
SELECT test_cs_free_unheld('pages') >= 0 AS cleared;
SELECT test_cs_unref_root('pages', pg_temp.h(pg_temp.p7()));
SELECT test_cs_free('pages', ARRAY[pg_temp.h(pg_temp.p7())]) AS freed;

SELECT test_chunk_format_refs(false);
