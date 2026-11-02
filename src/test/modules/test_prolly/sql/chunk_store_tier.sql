-- The chunk store conformance suite against "tier", the history store's
-- tier: its index on the server, its chunks' bytes in pack objects (here a
-- directory in the data directory, chunk_store.conf's), and its
-- transactional part.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET chunk_free_in_background = off;
\set store tier
\getenv abs_srcdir PG_ABS_SRCDIR
\set suite :abs_srcdir '/sql/chunk_store_suite.inc'
\i :suite
\set suite :abs_srcdir '/sql/chunk_store_transactional.inc'
\i :suite

-- what the tier keeps: a pack a transaction that put chunks, written as it
-- commits; none of one that aborted
SELECT test_chunk_format_refs(false);
-- (the packs the suite left all dead removed first)
VACUUM pg_history.tier_index;
SELECT count(*) AS packs0 FROM test_os_list('directory', 'pg_test_history_tier', 'packs/') \gset
BEGIN;
SELECT (test_cs_put('tier', convert_to('tier-a' || g, 'UTF8'))).stored
FROM generate_series(1, 3) g;
COMMIT;
SELECT count(*) - :packs0 AS one_pack_more FROM test_os_list('directory', 'pg_test_history_tier', 'packs/');
BEGIN;
SELECT (test_cs_put('tier', convert_to('tier-gone', 'UTF8'))).stored;
ROLLBACK;
SELECT count(*) - :packs0 AS still_one_more FROM test_os_list('directory', 'pg_test_history_tier', 'packs/');
SELECT test_cs_has('tier', substr(sha256(convert_to('tier-gone', 'UTF8')), 1, 20)) AS gone;
-- read back from its pack
SELECT convert_from(test_cs_get('tier', substr(sha256(convert_to('tier-a2', 'UTF8')), 1, 20)), 'UTF8') AS read_back;
-- the pack's chunks freed: VACUUM takes their entries out, the pack all
-- dead, its object removed
SELECT test_cs_free('tier', ARRAY(SELECT substr(sha256(convert_to('tier-a' || g, 'UTF8')), 1, 20)
	FROM generate_series(1, 3) g)) AS freed;
VACUUM pg_history.tier_index;
SELECT count(*) - :packs0 AS pack_removed FROM test_os_list('directory', 'pg_test_history_tier', 'packs/');
SELECT ok FROM test_cs_check('tier');

-- packs settled by VACUUM, once: committed (no transaction's id kept), or
-- orphaned and removed; the tier has a relfrozenxid (it had none), kept
-- no older than they
SELECT relfrozenxid AS frozen0 FROM pg_class WHERE oid = 'pg_history.tier_chunks'::regclass \gset
SELECT count(*) FILTER (WHERE state = 'made') AS made FROM test_tier_packs('pg_history.tier_chunks');
SELECT state, count(*) FROM test_tier_packs('pg_history.tier_chunks') GROUP BY state ORDER BY state;
SELECT relfrozenxid <> '0'::xid AS has_frozenxid, age(relfrozenxid) <= age(:'frozen0'::xid) AS not_older
FROM pg_class WHERE oid = 'pg_history.tier_chunks'::regclass;
-- and VACUUM of the tier itself settles them too
SELECT (test_cs_put('tier', convert_to('tier-settle', 'UTF8'))).stored;
SELECT count(*) FILTER (WHERE state = 'made') AS made_before FROM test_tier_packs('pg_history.tier_chunks');
VACUUM pg_history.tier_chunks;
SELECT count(*) FILTER (WHERE state = 'made') AS made_after FROM test_tier_packs('pg_history.tier_chunks');

-- a pack begun in a subtransaction rolled back is the top transaction's:
-- the chunks put after it, in it, committed, the pack kept
BEGIN;
SAVEPOINT s;
SELECT (test_cs_put('tier', convert_to('tier-sub-gone', 'UTF8'))).stored;
ROLLBACK TO s;
SELECT (test_cs_put('tier', convert_to('tier-sub-kept', 'UTF8'))).stored;
COMMIT;
SELECT test_cs_ref_root('tier', substr(sha256(convert_to('tier-sub-kept', 'UTF8')), 1, 20));
VACUUM pg_history.tier_index;
SELECT convert_from(test_cs_get('tier', substr(sha256(convert_to('tier-sub-kept', 'UTF8')), 1, 20)), 'UTF8') AS kept,
	test_cs_has('tier', substr(sha256(convert_to('tier-sub-gone', 'UTF8')), 1, 20)) AS gone;
SELECT ok FROM test_cs_check('tier');

-- a dump of the tier's chunks: no bundle, none of them (a bundle's chunks
-- are dumped from the tier, and the bundle after them)
SELECT count(*) FILTER (WHERE roots IS NOT NULL) AS bundles FROM pg_history_export();

-- a pack's table, at its end: each chunk in it, by hash, its bytes there
-- hashing to it
SELECT count(*) FROM generate_series(1, 7) g,
	LATERAL test_cs_put('tier', convert_to(repeat('table' || g, g * 10), 'UTF8'));
SELECT max(id) AS last_pack FROM test_tier_packs('pg_history.tier_chunks') WHERE state <> 'removed' \gset
SELECT count(*) AS entries,
	count(*) FILTER (WHERE test_cs_get('tier', t.hash) IS NOT NULL
		AND substr(sha256(test_cs_get('tier', t.hash)), 1, 20) = t.hash
		AND length(test_cs_get('tier', t.hash)) = t.len) AS each_its_chunk,
	bool_and(t.hash IN (SELECT substr(sha256(convert_to(repeat('table' || g, g * 10), 'UTF8')), 1, 20)
		FROM generate_series(1, 7) g)) AS all_put,
	(SELECT bool_and(a < b) FROM (SELECT hash a, lead(hash) OVER (ORDER BY ord) b
		FROM test_tier_pack_table('pg_history.tier_chunks', :last_pack) WITH ORDINALITY x(hash, off, len, ord)) y
		WHERE b IS NOT NULL) AS by_hash
FROM test_tier_pack_table('pg_history.tier_chunks', :last_pack) t;

-- pack objects malformed: each refused by the reader of tables (a footer:
-- entries, version, bytes of chunks, magic; an entry: hash, offset, length)
CREATE FUNCTION pg_temp.footer(n int, version int, data_len int8, magic int8 DEFAULT 5498985688571396939)
RETURNS bytea LANGUAGE sql AS 'SELECT int4send(n) || int4send(version) || int8send(data_len) || int8send(magic)';
CREATE FUNCTION pg_temp.entry(d bytea, off int, len int) RETURNS bytea LANGUAGE sql
	AS 'SELECT substr(sha256(d), 1, 20) || int4send(off) || int4send(len)';
CREATE FUNCTION pg_temp.pack(name text, data bytea) RETURNS void LANGUAGE sql
	AS $$SELECT test_os_put('directory', 'pg_test_history_tier', 'bad/' || name, data)$$;
CREATE FUNCTION pg_temp.read(name text) RETURNS SETOF record LANGUAGE sql
	AS $$SELECT hash, off, len FROM test_os_pack_table('directory', 'pg_test_history_tier', 'bad/' || name)$$;
-- (a good one first: two chunks, by hash)
SELECT pg_temp.pack('good', '\x0102'::bytea || '\x03'::bytea ||
	(SELECT string_agg(e, '' ORDER BY e) FROM (VALUES (pg_temp.entry('\x0102', 0, 2)), (pg_temp.entry('\x03', 2, 1))) v(e)) ||
	pg_temp.footer(2, 1, 3));
SELECT off, len FROM test_os_pack_table('directory', 'pg_test_history_tier', 'bad/good') ORDER BY off;
SELECT pg_temp.pack('short', '\x0102');
SELECT * FROM pg_temp.read('short') AS (h bytea, o int8, l int8);
SELECT pg_temp.pack('magic', pg_temp.footer(0, 1, 0, 1));
SELECT * FROM pg_temp.read('magic') AS (h bytea, o int8, l int8);
SELECT pg_temp.pack('version', pg_temp.footer(0, 2, 0));
SELECT * FROM pg_temp.read('version') AS (h bytea, o int8, l int8);
SELECT pg_temp.pack('size', '\x01' || pg_temp.footer(0, 1, 2));
SELECT * FROM pg_temp.read('size') AS (h bytea, o int8, l int8);
-- (bytes of chunks so many their sum with the rest wraps to the size)
SELECT pg_temp.pack('wrap', '\x01' || pg_temp.footer(1, 1, -27));
SELECT * FROM pg_temp.read('wrap') AS (h bytea, o int8, l int8);
SELECT pg_temp.pack('entries', '\x01' || pg_temp.footer(1000000, 1, 1));
SELECT * FROM pg_temp.read('entries') AS (h bytea, o int8, l int8);
SELECT pg_temp.pack('past', '\x0102' || pg_temp.entry('\x0102', 1, 2) || pg_temp.footer(1, 1, 2));
SELECT * FROM pg_temp.read('past') AS (h bytea, o int8, l int8);
SELECT pg_temp.pack('order', '\x0102'::bytea || '\x03'::bytea ||
	(SELECT string_agg(e, '' ORDER BY e DESC) FROM (VALUES (pg_temp.entry('\x0102', 0, 2)), (pg_temp.entry('\x03', 2, 1))) v(e)) ||
	pg_temp.footer(2, 1, 3));
SELECT * FROM pg_temp.read('order') AS (h bytea, o int8, l int8);

-- a chunk put, its savepoint rolled back, and put again: two entries of
-- its hash, each its bytes; the index's the live one
BEGIN;
SAVEPOINT s;
SELECT (test_cs_put('tier', convert_to('twice in a pack', 'UTF8'))).stored;
ROLLBACK TO s;
SELECT (test_cs_put('tier', convert_to('twice in a pack', 'UTF8'))).stored;
COMMIT;
SELECT max(id) AS twice_pack FROM test_tier_packs('pg_history.tier_chunks') WHERE state <> 'removed' \gset
SELECT count(*) AS entries, count(DISTINCT off) AS places
FROM test_tier_pack_table('pg_history.tier_chunks', :twice_pack)
WHERE hash = substr(sha256(convert_to('twice in a pack', 'UTF8')), 1, 20);

-- a tier is pg_history's alone (the tests' made there, and truncated and
-- dropped, as one allowed to change the system's catalogs)
CREATE TABLE public_tier () USING prolly_tier;
SET allow_system_table_mods = on;
CREATE TABLE pg_history.my_tier () USING prolly_tier;
CREATE TABLE pg_history.my_tier_index () USING prolly_chunk_index;

-- a tier with packs is not truncated, nor dropped (their objects would be
-- left in the store, named by no catalog); emptied, it is
SELECT test_chunk_put('pg_history.my_tier', 'pg_history.my_tier_index', substr(sha256('\x0a'), 1, 20), '\x0a');
SELECT test_chunk_get('pg_history.my_tier', 'pg_history.my_tier_index', substr(sha256('\x0a'), 1, 20));
TRUNCATE pg_history.my_tier, pg_history.my_tier_index;
DROP TABLE pg_history.my_tier;
CREATE TEMP TABLE names0 AS
	SELECT name FROM test_os_list('directory', 'pg_test_history_tier', 'packs/');
SELECT test_chunk_free('pg_history.my_tier', 'pg_history.my_tier_index', substr(sha256('\x0a'), 1, 20));
VACUUM pg_history.my_tier_index;
SELECT count(*) FILTER (WHERE state <> 'removed') AS packs_left FROM test_tier_packs('pg_history.my_tier');
SELECT count(*) AS its_object_gone FROM test_os_list('directory', 'pg_test_history_tier', 'packs/') n
WHERE n.name NOT IN (SELECT name FROM names0);

-- its stamp, in its packs' names, a new one after TRUNCATE (the one kept in
-- the relcache let go)
TRUNCATE pg_history.my_tier, pg_history.my_tier_index;
SELECT test_chunk_put('pg_history.my_tier', 'pg_history.my_tier_index', substr(sha256('\x0b'), 1, 20), '\x0b');
SELECT test_chunk_get('pg_history.my_tier', 'pg_history.my_tier_index', substr(sha256('\x0b'), 1, 20));
SELECT count(*) AS new_names,
	count(*) FILTER (WHERE split_part(n.name, '/', 2) IN
		(SELECT split_part(o.name, '/', 2) FROM names0 o)) AS of_a_stamp_before
FROM test_os_list('directory', 'pg_test_history_tier', 'packs/') n
WHERE n.name NOT IN (SELECT name FROM names0);

-- a tier whose storage is this transaction's (made, or truncated, in it)
-- takes no chunk until it commits: aborted, its catalog would go, and with
-- it what knows the objects of packs written; the store's objects as they
-- were
CREATE TEMP TABLE names1 AS
	SELECT name FROM test_os_list('directory', 'pg_test_history_tier', 'packs/');
BEGIN;
CREATE TABLE pg_history.my_tier2 () USING prolly_tier;
CREATE TABLE pg_history.my_tier2_index () USING prolly_chunk_index;
-- (65 MB: a pack filled would be written as it goes)
SELECT count(*) FROM generate_series(1, 65) g,
	LATERAL test_chunk_put('pg_history.my_tier2', 'pg_history.my_tier2_index',
		substr(sha256(convert_to(rpad(g::text, 1048576, '.'), 'UTF8')), 1, 20),
		convert_to(rpad(g::text, 1048576, '.'), 'UTF8'));
ROLLBACK;
CREATE TABLE pg_history.my_tier4 () USING prolly_tier;
CREATE TABLE pg_history.my_tier4_index () USING prolly_chunk_index;
BEGIN;
TRUNCATE pg_history.my_tier4, pg_history.my_tier4_index;
SELECT test_chunk_put('pg_history.my_tier4', 'pg_history.my_tier4_index', substr(sha256('\x0d'), 1, 20), '\x0d');
ROLLBACK;
DROP TABLE pg_history.my_tier4, pg_history.my_tier4_index;
SELECT count(*) AS objects_made FROM test_os_list('directory', 'pg_test_history_tier', 'packs/') n
WHERE n.name NOT IN (SELECT name FROM names1);

-- no chunk through an index made in this transaction either: aborted, the
-- index would go, the tier's packs left with none to be freed through
CREATE TABLE pg_history.my_tier5 () USING prolly_tier;
BEGIN;
CREATE TABLE pg_history.my_tier5_index () USING prolly_chunk_index;
SELECT test_chunk_put('pg_history.my_tier5', 'pg_history.my_tier5_index', substr(sha256('\x0e05'), 1, 20), '\x0e05');
ROLLBACK;
SELECT count(*) AS packs FROM test_tier_packs('pg_history.my_tier5');
DROP TABLE pg_history.my_tier5;

-- the index bound to a tier with packs is not dropped, nor truncated (the
-- tier's packs could not be freed without it); and a tier is not moved out
-- of pg_history
DROP TABLE pg_history.my_tier_index;
TRUNCATE pg_history.my_tier_index;
CREATE SCHEMA tier_elsewhere;
ALTER TABLE pg_history.my_tier SET SCHEMA tier_elsewhere;
DROP SCHEMA tier_elsewhere;

-- a tier dropped in the transaction that put chunks in it: refused (its
-- pack, being made, is one)
BEGIN;
SELECT test_chunk_put('pg_history.my_tier', 'pg_history.my_tier_index', substr(sha256('\x0c'), 1, 20), '\x0c');
DROP TABLE pg_history.my_tier;
ROLLBACK;
-- (emptied, dropped)
VACUUM pg_history.my_tier_index;
SELECT test_chunk_free('pg_history.my_tier', 'pg_history.my_tier_index', substr(sha256('\x0b'), 1, 20));
VACUUM pg_history.my_tier_index;
DROP TABLE pg_history.my_tier, pg_history.my_tier_index;
RESET allow_system_table_mods;

-- repacking: a pack with more than the fraction of its bytes dead is a
-- segment to compact; compaction moves its live chunks into a pack of its
-- transaction, and the old one, all dead then, is removed
CREATE FUNCTION pg_temp.rp(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT convert_to(rpad('repack' || i, 1000, '.'), 'UTF8')$$;
SELECT count(*) FROM generate_series(1, 10) g, LATERAL test_cs_put('tier', pg_temp.rp(g));
SELECT max(id) AS rp_pack FROM test_tier_packs('pg_history.tier_chunks') WHERE state <> 'removed' \gset
SELECT count(*) FROM generate_series(1, 10) g,
	LATERAL test_cs_ref_root('tier', substr(sha256(pg_temp.rp(g)), 1, 20));
SELECT count(*) FROM generate_series(1, 6) g,
	LATERAL test_cs_unref_root('tier', substr(sha256(pg_temp.rp(g)), 1, 20));
SELECT test_cs_free('tier', ARRAY(SELECT substr(sha256(pg_temp.rp(g)), 1, 20) FROM generate_series(1, 6) g)) AS freed;
VACUUM pg_history.tier_index;
SELECT state, size, dead FROM test_tier_packs('pg_history.tier_chunks') WHERE id = :rp_pack;
SELECT compactable, worth FROM test_chunk_log_segments('pg_history.tier_chunks', 0.5);
SELECT compactable AS above_fraction FROM test_chunk_log_segments('pg_history.tier_chunks', 0.7);
SELECT segments, chunks, bytes FROM test_chunk_compact('pg_history.tier_chunks', 'pg_history.tier_index', 0.5, false);
VACUUM pg_history.tier_index;
SELECT state FROM test_tier_packs('pg_history.tier_chunks') WHERE id = :rp_pack;
SELECT count(*) FILTER (WHERE test_cs_get('tier', substr(sha256(pg_temp.rp(g)), 1, 20)) = pg_temp.rp(g)) AS live_read
FROM generate_series(7, 10) g;
SELECT count(*) AS entries FROM test_tier_packs('pg_history.tier_chunks') p,
	LATERAL test_tier_pack_table('pg_history.tier_chunks', p.id) t
WHERE p.id > :rp_pack AND p.state <> 'removed' AND p.size > 0
	AND t.hash IN (SELECT substr(sha256(pg_temp.rp(g)), 1, 20) FROM generate_series(7, 10) g);
SELECT ok FROM test_cs_check('tier');
SELECT count(*) FROM generate_series(7, 10) g,
	LATERAL test_cs_unref_root('tier', substr(sha256(pg_temp.rp(g)), 1, 20));
SELECT test_cs_free('tier', ARRAY(SELECT substr(sha256(pg_temp.rp(g)), 1, 20) FROM generate_series(7, 10) g)) AS freed;

-- a repack aborted: its pack an orphan, removed; the chunks where they were
SELECT count(*) FROM generate_series(31, 34) g, LATERAL test_cs_put('tier', pg_temp.rp(g));
SELECT count(*) FROM generate_series(31, 34) g,
	LATERAL test_cs_ref_root('tier', substr(sha256(pg_temp.rp(g)), 1, 20));
SELECT max(id) AS ab_pack FROM test_tier_packs('pg_history.tier_chunks') WHERE state <> 'removed' \gset
SELECT test_cs_unref_root('tier', substr(sha256(pg_temp.rp(31)), 1, 20)),
	test_cs_unref_root('tier', substr(sha256(pg_temp.rp(32)), 1, 20));
SELECT test_cs_free('tier', ARRAY[substr(sha256(pg_temp.rp(31)), 1, 20), substr(sha256(pg_temp.rp(32)), 1, 20)]) AS freed;
VACUUM pg_history.tier_index;
SELECT dead AS dead_before FROM test_tier_packs('pg_history.tier_chunks') WHERE id = :ab_pack;
BEGIN;
SELECT segments, chunks FROM test_chunk_compact('pg_history.tier_chunks', 'pg_history.tier_index', 0.5, false);
ROLLBACK;
VACUUM pg_history.tier_index;
SELECT state FROM test_tier_packs('pg_history.tier_chunks') WHERE id = :ab_pack;
SELECT count(*) AS orphans_left FROM test_tier_packs('pg_history.tier_chunks') WHERE id > :ab_pack AND state IN ('made', 'orphaned');
-- (the repack's pack was begun, an orphan, never written: an abort writes
-- none, and removed; the chunks' pack as it was)
SELECT count(*) AS begun_removed FROM test_tier_packs('pg_history.tier_chunks')
WHERE id > :ab_pack AND state = 'removed' AND size = 0;
SELECT dead FROM test_tier_packs('pg_history.tier_chunks') WHERE id = :ab_pack;
SELECT count(*) AS still_in_its_table FROM test_tier_pack_table('pg_history.tier_chunks', :ab_pack)
WHERE hash IN (SELECT substr(sha256(pg_temp.rp(g)), 1, 20) FROM generate_series(33, 34) g);
SELECT count(*) FILTER (WHERE test_cs_get('tier', substr(sha256(pg_temp.rp(g)), 1, 20)) = pg_temp.rp(g)) AS read_back
FROM generate_series(33, 34) g;
SELECT ok FROM test_cs_check('tier');

-- a repack of at most 64 packs: 66 packs partly dead, 64 compacted
CREATE PROCEDURE pg_temp.small_packs() LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1 .. 66 LOOP
		PERFORM test_cs_put('tier', pg_temp.rp(1000 + 2 * i));
		PERFORM test_cs_put('tier', pg_temp.rp(1001 + 2 * i));
		PERFORM test_cs_ref_root('tier', substr(sha256(pg_temp.rp(1001 + 2 * i)), 1, 20));
		COMMIT;
	END LOOP;
END $$;
CALL pg_temp.small_packs();
SELECT test_cs_free('tier', ARRAY(SELECT substr(sha256(pg_temp.rp(1000 + 2 * i)), 1, 20) FROM generate_series(1, 66) i)) AS freed;
VACUUM pg_history.tier_index;
SELECT compactable >= 66 AS candidates FROM test_chunk_log_segments('pg_history.tier_chunks', 0.4);
SELECT segments FROM test_chunk_compact('pg_history.tier_chunks', 'pg_history.tier_index', 0.4, false);
