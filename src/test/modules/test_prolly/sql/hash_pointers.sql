-- Hash pointers: TOAST pointers that name a value by content, a value tree
-- in the database's chunk store.  Detoasting fetches the value, a slice
-- fetches its bytes, sizes come from the pointer, and equal values are
-- one tree.  Text in another encoding and jsonb are stored in their
-- canonical form, and fetched back in their own.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE TEMP TABLE v AS
SELECT string_agg(md5(g::text), '') AS t,
	   decode(string_agg(md5(g::text), ''), 'hex') AS b,
	   jsonb_build_object('n', 7, 'tags', jsonb_agg(md5(g::text))) AS j
FROM generate_series(1, 3000) g;

-- Each read back whole, through a pointer whose form says what its tree
-- holds: the value's bytes, or a document's canonical bytes.
SELECT (test_toast_pointer(test_toast_hash_pointer(t))).form AS text_form,
	   (test_toast_pointer(test_toast_hash_pointer(b))).form AS bytea_form,
	   (test_toast_pointer(test_toast_hash_pointer(j))).form AS jsonb_form,
	   test_toast_pointer(t) IS NULL AS not_a_pointer,
	   test_toast_hash_pointer(t) = t AS text_same,
	   test_toast_hash_pointer(b) = b AS bytea_same,
	   test_toast_hash_pointer(j) = j AS jsonb_same,
	   test_toast_hash_pointer(j) -> 'n' AS jsonb_field
FROM v;

-- Sizes from the pointer, no fetch: raw and stored (never compressed).
SELECT octet_length(test_toast_hash_pointer(t)) = octet_length(t) AS raw_size,
	   pg_column_size(test_toast_hash_pointer(b)) = octet_length(b) AS stored_size,
	   pg_column_compression(test_toast_hash_pointer(t)) IS NULL AS no_compression,
	   pg_column_toast_chunk_id(test_toast_hash_pointer(t)) IS NULL AS no_chunk_id
FROM v;

-- Slices at the start, inside, across chunks, at and past the end.
SELECT count(*) FILTER (WHERE substr(test_toast_hash_pointer(t), s, n) = substr(t, s, n)) AS text_slices,
	   count(*) FILTER (WHERE substr(test_toast_hash_pointer(b), s, n) = substr(b, s, n)) AS bytea_slices,
	   count(*) AS slices
FROM v, (VALUES (1, 10), (1, 0), (4000, 9000), (95990, 10), (96000, 5),
		 (96001, 5), (200000, 3), (48000, 100000)) s(s, n);

-- Equal values, one tree: their pointers name the same root, and a value
-- equal but for its type's form doesn't.
SELECT test_toast_pointer(test_toast_hash_pointer(t)) =
	   test_toast_pointer(test_toast_hash_pointer(t || '')) AS same_pointer,
	   (test_toast_pointer(test_toast_hash_pointer(t))).root =
	   (test_toast_pointer(test_toast_hash_pointer(convert_to(t, 'UTF8')))).root
	   AS text_is_its_bytes,
	   (test_toast_pointer(test_toast_hash_pointer(j))).root <>
	   (test_toast_pointer(test_toast_hash_pointer(convert_to(j::text, 'UTF8')))).root
	   AS jsonb_is_not
FROM v;

-- In a LATIN1 database, text is stored in UTF-8 and read back in LATIN1,
-- whole or sliced; jsonb in its canonical form.
SELECT current_database() AS db \gset
CREATE DATABASE hp_latin1 ENCODING 'LATIN1' LC_COLLATE 'C' LC_CTYPE 'C'
  TEMPLATE template0;
\c hp_latin1
SET client_encoding = 'UTF8';
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE TEMP TABLE v AS
SELECT repeat('é' || g, 2000) AS t, jsonb_build_object('é', repeat('ü', 5000), 'a', g) AS j
FROM generate_series(1, 2) g;
SELECT (test_toast_pointer(test_toast_hash_pointer(t))).form AS text_form,
	   test_toast_hash_pointer(t) = t AS text_same,
	   substr(test_toast_hash_pointer(t), 3, 7) = substr(t, 3, 7) AS text_slice,
	   octet_length(test_toast_hash_pointer(t)) = octet_length(t) AS raw_size,
	   (test_toast_pointer(test_toast_hash_pointer(t))).root =
	   (test_toast_pointer(test_toast_hash_pointer(convert_to(t, 'UTF8')))).root
	   AS stored_in_utf8,
	   (test_toast_pointer(test_toast_hash_pointer(j))).form AS jsonb_form,
	   test_toast_hash_pointer(j) = j AS jsonb_same
FROM v ORDER BY t;
\c :db
DROP DATABASE hp_latin1;

-- In EUC_JP, 0xADF0 and 0xA2E2 are one Unicode character, which converts
-- back to 0xA2E2: text holding 0xADF0 is stored as its bytes, and read
-- back as they were; text without one is stored in UTF-8.
CREATE DATABASE hp_euc_jp ENCODING 'EUC_JP' LC_COLLATE 'C' LC_CTYPE 'C'
  TEMPLATE template0;
\c hp_euc_jp
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE TEMP TABLE v AS
SELECT repeat(convert_from('\xadf0'::bytea, 'EUC_JP') || 'x', 2000) AS t,
	   repeat(convert_from('\xa2e2'::bytea, 'EUC_JP') || 'x', 2000) AS u;
SELECT (test_toast_pointer(test_toast_hash_pointer(t))).form AS no_round_trip_form,
	   convert_to(test_toast_hash_pointer(t), 'EUC_JP') = convert_to(t, 'EUC_JP') AS same_bytes,
	   (test_toast_pointer(test_toast_hash_pointer(u))).form AS round_trip_form,
	   convert_to(test_toast_hash_pointer(u), 'EUC_JP') = convert_to(u, 'EUC_JP') AS u_same_bytes
FROM v;
\c :db
DROP DATABASE hp_euc_jp;

-- In SQL_ASCII, text's bytes are kept as they are, whatever they hold.
CREATE DATABASE hp_sql_ascii ENCODING 'SQL_ASCII' LC_COLLATE 'C' LC_CTYPE 'C'
  TEMPLATE template0;
\c hp_sql_ascii
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE TEMP TABLE v AS
SELECT repeat(convert_from('\xe9ff'::bytea, 'SQL_ASCII') || 'x', 2000) AS t;
SELECT (test_toast_pointer(test_toast_hash_pointer(t))).form AS sql_ascii_form,
	   test_toast_hash_pointer(t) = t AS same
FROM v;
\c :db
DROP DATABASE hp_sql_ascii;
