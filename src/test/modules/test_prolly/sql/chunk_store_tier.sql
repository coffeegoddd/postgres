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

-- a dump refuses the tier's chunks, not yet dumped
SELECT count(*) FROM pg_history_export();

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
CREATE FUNCTION pg_temp.footer(n int, version int, data_len int8, magic int8 DEFAULT 5423250189635965004)
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

-- a tier the test makes: its stamp, in its packs' names, a new one after
-- TRUNCATE (the one kept in the relcache let go)
CREATE TABLE my_tier () USING prolly_tier;
CREATE TABLE my_tier_index () USING prolly_chunk_index;
SELECT test_chunk_put('my_tier', 'my_tier_index', substr(sha256('\x0a'), 1, 20), '\x0a');
SELECT test_chunk_get('my_tier', 'my_tier_index', substr(sha256('\x0a'), 1, 20));
CREATE TEMP TABLE names0 AS
	SELECT name FROM test_os_list('directory', 'pg_test_history_tier', 'packs/');
TRUNCATE my_tier, my_tier_index;
SELECT test_chunk_put('my_tier', 'my_tier_index', substr(sha256('\x0b'), 1, 20), '\x0b');
SELECT test_chunk_get('my_tier', 'my_tier_index', substr(sha256('\x0b'), 1, 20));
SELECT count(*) AS new_names,
	count(*) FILTER (WHERE split_part(n.name, '/', 2) IN
		(SELECT split_part(o.name, '/', 2) FROM names0 o)) AS of_a_stamp_before
FROM test_os_list('directory', 'pg_test_history_tier', 'packs/') n
WHERE n.name NOT IN (SELECT name FROM names0);

-- and TRUNCATE in the transaction that made the tier (its storage cut in
-- place): the pack begun before it gone with it
BEGIN;
CREATE TABLE my_tier2 () USING prolly_tier;
CREATE TABLE my_tier2_index () USING prolly_chunk_index;
SELECT test_chunk_put('my_tier2', 'my_tier2_index', substr(sha256('\x0d'), 1, 20), '\x0d');
TRUNCATE my_tier2, my_tier2_index;
SELECT test_chunk_put('my_tier2', 'my_tier2_index', substr(sha256('\x0e'), 1, 20), '\x0e');
COMMIT;
SELECT test_chunk_get('my_tier2', 'my_tier2_index', substr(sha256('\x0e'), 1, 20)),
	test_chunk_has('my_tier2', 'my_tier2_index', substr(sha256('\x0d'), 1, 20)) AS gone;
SELECT id, state FROM test_tier_packs('my_tier2');
DROP TABLE my_tier2, my_tier2_index;

-- TRUNCATE (new storage) in a transaction that put chunks in the tier:
-- refused (its pack's entry is the storage's)
BEGIN;
SELECT test_chunk_put('my_tier', 'my_tier_index', substr(sha256('\x0f'), 1, 20), '\x0f');
TRUNCATE my_tier, my_tier_index;
ROLLBACK;

-- a tier dropped in the transaction that put chunks in it: named as it
-- commits
BEGIN;
SELECT test_chunk_put('my_tier', 'my_tier_index', substr(sha256('\x0c'), 1, 20), '\x0c');
DROP TABLE my_tier;
COMMIT;
DROP TABLE my_tier, my_tier_index;
