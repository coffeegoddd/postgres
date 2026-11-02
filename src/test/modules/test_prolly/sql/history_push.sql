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
INSERT INTO pg_history.bundles VALUES ('dupx', ARRAY[pg_temp.h('\x0a11ff'), pg_temp.h('\x0a11ff')], 0);
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
-- (a fetch, a push and a release in a read-only transaction, and a fetch
-- by one not a superuser, granted it: refused)
BEGIN READ ONLY;
SELECT pg_history_fetch('b8');
ROLLBACK;
BEGIN READ ONLY;
SELECT pg_history_push('bx', ARRAY[pg_temp.h('\x0a1701')]);
ROLLBACK;
BEGIN READ ONLY;
SELECT pg_history_release('b8');
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

-- manifests: each bundle's, in the tier's store, which lists the bundles
-- from it alone; a push aborted leaves an orphan, removed by VACUUM; a
-- release's manifest stays until VACUUM, its release committed, removes it
SELECT (test_cs_put('pages', '\x0a1901')).stored, (test_cs_put('pages', '\x0a1902')).stored;
SELECT pg_history_push('b9', ARRAY[pg_temp.h('\x0a1901')]) AS copied;
SELECT name, roots = ARRAY[pg_temp.h('\x0a1901')] AS its_roots FROM pg_history_tier_bundles() ORDER BY name;
SELECT manifest > 0 AS has_manifest FROM pg_history.bundles WHERE name = 'b9';
BEGIN;
SELECT pg_history_push('b10', ARRAY[pg_temp.h('\x0a1902')]) AS copied;
ROLLBACK;
SELECT name FROM pg_history_tier_bundles() ORDER BY name;
VACUUM pg_history.tier_index;
SELECT name FROM pg_history_tier_bundles() ORDER BY name;
-- (a release aborted: its manifest kept)
BEGIN;
SELECT pg_history_release('b9') AS freed;
ROLLBACK;
VACUUM pg_history.tier_index;
SELECT name FROM pg_history_tier_bundles() ORDER BY name;
SELECT pg_history_release('b9') AS freed;
SELECT name FROM pg_history_tier_bundles() ORDER BY name;
VACUUM pg_history.tier_index;
SELECT count(*) AS left_after_vacuum FROM pg_history_tier_bundles();
SELECT test_cs_free('pages', ARRAY[pg_temp.h('\x0a1902')]) AS freed;

-- a release in a subtransaction rolled back: its manifest the bundle's
-- still; a push in one: its manifest an orphan, removed; a release after
-- one aborted, at once
SELECT (test_cs_put('pages', '\x0a1903')).stored, (test_cs_put('pages', '\x0a1904')).stored;
SELECT pg_history_push('b11', ARRAY[pg_temp.h('\x0a1903')]) AS copied;
BEGIN;
SAVEPOINT s;
SELECT pg_history_release('b11') AS freed;
ROLLBACK TO s;
COMMIT;
VACUUM pg_history.tier_index;
SELECT name FROM pg_history_tier_bundles() ORDER BY name;
BEGIN;
SAVEPOINT s;
SELECT pg_history_push('b12', ARRAY[pg_temp.h('\x0a1904')]) AS copied;
ROLLBACK TO s;
COMMIT;
VACUUM pg_history.tier_index;
SELECT name FROM pg_history_tier_bundles() ORDER BY name;
BEGIN;
SELECT pg_history_release('b11') AS freed;
ROLLBACK;
SELECT pg_history_release('b11') AS freed_at_once;
VACUUM pg_history.tier_index;
SELECT count(*) AS left FROM pg_history_tier_bundles();
SELECT test_cs_free('pages', ARRAY[pg_temp.h('\x0a1904')]) AS freed;

-- a bundle's row names one manifest, its own, a tier's entry's number
SELECT (test_cs_put('pages', '\x0a1905')).stored, (test_cs_put('pages', '\x0a1906')).stored;
SELECT pg_history_push('b13', ARRAY[pg_temp.h('\x0a1905')]) AS copied;
INSERT INTO pg_history.bundles VALUES ('b14', ARRAY[pg_temp.h('\x0a1905')], -5);
-- (terse: the number another bundle's, whatever it is)
\set VERBOSITY terse
INSERT INTO pg_history.bundles SELECT 'b14', roots, manifest FROM pg_history.bundles WHERE name = 'b13';
\set VERBOSITY default
SELECT kind FROM test_tier_packs('pg_history.tier_chunks')
WHERE id = (SELECT manifest FROM pg_history.bundles WHERE name = 'b13');

-- an object under manifests/ that is no manifest: the listing refuses it
SELECT split_part(name, '/', 2) AS mstamp FROM test_os_list('directory', 'pg_test_history_tier', 'manifests/') LIMIT 1 \gset
SELECT test_os_put('directory', 'pg_test_history_tier', 'manifests/' || :'mstamp' || '/0000000000000000/4000000000',
	int8send(5498985688337368140) || int4send(1) || int4send(1) || int4send(0) || int4send(0) || '\x41'::bytea);
SELECT count(*) FROM pg_history_tier_bundles();
SELECT test_os_remove('directory', 'pg_test_history_tier', 'manifests/' || :'mstamp' || '/0000000000000000/4000000000');
-- (and one naming its bundle with a NUL)
SELECT test_os_put('directory', 'pg_test_history_tier', 'manifests/' || :'mstamp' || '/0000000000000000/4000000001',
	int8send(5498985688337368140) || int4send(1) || int4send(2) || int4send(1) || int4send(0) || '\x4100'::bytea ||
	substr(sha256('\x01'), 1, 20));
SELECT count(*) FROM pg_history_tier_bundles();
SELECT test_os_remove('directory', 'pg_test_history_tier', 'manifests/' || :'mstamp' || '/0000000000000000/4000000001');
SELECT pg_history_release('b13') AS freed;
VACUUM pg_history.tier_index;

-- a pack begun in a subtransaction rolled back, the transaction committed:
-- written, the tier locked again as it is
BEGIN;
SAVEPOINT s;
SELECT pg_history_push('b15', ARRAY[pg_temp.h('\x0a1906')]) AS copied;
ROLLBACK TO s;
COMMIT;
SELECT count(*) AS no_b15 FROM pg_history.bundles WHERE name = 'b15';
SELECT test_cs_free('pages', ARRAY[pg_temp.h('\x0a1906')]) AS freed;
VACUUM pg_history.tier_index;

-- a fetch reads a pack as the push laid it out (a chunk's children
-- together): a few reads of the tier's objects for a tree, not one a chunk
CREATE FUNCTION pg_temp.tl(g int) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT convert_to(rpad('tree' || g, 200, '.'), 'UTF8')$$;
CREATE FUNCTION pg_temp.tm(m int) RETURNS bytea LANGUAGE sql IMMUTABLE AS $$
	SELECT '\x52'::bytea || set_byte('\x00'::bytea, 0, 5) ||
		(SELECT string_agg(pg_temp.h(pg_temp.tl(m * 5 + g)), ''::bytea ORDER BY g) FROM generate_series(0, 4) g)$$;
CREATE FUNCTION pg_temp.tr() RETURNS bytea LANGUAGE sql IMMUTABLE AS $$
	SELECT '\x52'::bytea || set_byte('\x00'::bytea, 0, 3) ||
		(SELECT string_agg(pg_temp.h(pg_temp.tm(m)), ''::bytea ORDER BY m) FROM generate_series(0, 2) m)$$;
SELECT count(*) FROM generate_series(0, 14) g, LATERAL test_cs_put('pages', pg_temp.tl(g));
SELECT count(*) FROM generate_series(0, 2) m, LATERAL test_cs_put('pages', pg_temp.tm(m));
SELECT (test_cs_put('pages', pg_temp.tr())).stored;
SELECT pg_history_push('tree', ARRAY[pg_temp.h(pg_temp.tr())]) AS copied;
-- (laid out as walked: each of the three's leaves together, it after them)
WITH t AS (
	SELECT t.hash, t.off, t.len FROM test_tier_packs('pg_history.tier_chunks') p,
		LATERAL test_tier_pack_table('pg_history.tier_chunks', p.id) t
	WHERE p.state <> 'removed' AND p.kind = 'pack' AND p.size > 0 AND EXISTS (
		SELECT FROM test_tier_pack_table('pg_history.tier_chunks', p.id) r
		WHERE r.hash = pg_temp.h(pg_temp.tr())))
SELECT bool_and(mid.off = (SELECT max(l.off + l.len) FROM t l
		WHERE l.hash IN (SELECT pg_temp.h(pg_temp.tl(m * 5 + g)) FROM generate_series(0, 4) g))
	AND mid.off - 1000 = (SELECT min(l.off) FROM t l
		WHERE l.hash IN (SELECT pg_temp.h(pg_temp.tl(m * 5 + g)) FROM generate_series(0, 4) g)))
	AS leaves_together
FROM generate_series(0, 2) m JOIN t mid ON mid.hash = pg_temp.h(pg_temp.tm(m));
BEGIN;
SELECT test_tier_object_reads() AS reads0 \gset
SELECT pg_history_fetch('tree') AS fetched;
SELECT test_tier_object_reads() - :reads0 <= 2 * 3 + 1 AS few_reads,
	test_tier_object_reads() - :reads0 AS reads;
ROLLBACK;
SELECT pg_history_release('tree') AS freed;
VACUUM pg_history.tier_index;

SELECT test_chunk_format_refs(false);
