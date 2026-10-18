-- jsonb's canonical bytes are a version byte and the document as jsonb
-- stores it, its strings in UTF-8: equal documents have equal bytes
-- however they were written, a database of another encoding gives the
-- same bytes, and bytes that aren't such a document are refused.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

-- A version byte, then the bytes the value holds (after its 4-byte header).
SELECT get_byte(prolly_canonical_value(j), 0) AS version,
	   length(prolly_canonical_value(j)) = pg_column_size(j) - 3 AS as_stored
FROM (SELECT '{"a": [1, "two", null, true, 3.25]}'::jsonb AS j) s;

-- Equal documents, equal bytes: keys in any order, a key given twice
-- (the last wins), spacing.
SELECT prolly_canonical_value('{"b": 1, "a": {"y": 2, "x": [1, 2]}}'::jsonb) =
	   prolly_canonical_value('{"a":{"x":[1,2],"y":2},"b":1}'::jsonb) AS key_order,
	   prolly_canonical_value('{"a": 1, "a": 2}'::jsonb) =
	   prolly_canonical_value('{"a": 2}'::jsonb) AS last_key_wins,
	   prolly_canonical_value('{"a": 1}'::jsonb) <>
	   prolly_canonical_value('{"a": 1.0}'::jsonb) AS numeric_scale_kept;

-- Round trips: scalars, empty containers, nesting, more children than an
-- offset's stride (32), non-ASCII strings.
SELECT j, prolly_canonical_round_trip(j) = j AS same
FROM (VALUES ('1'::jsonb), ('"s"'), ('null'), ('true'), ('[]'), ('{}'),
			 ('[[[{"deep": [[]]}]]]'), ('{"é": "ü", "a": "€"}'),
			 ('-12345678901234567890.0000000001')) v(j);
SELECT prolly_canonical_round_trip(j) = j AS wide_same
FROM (SELECT jsonb_object_agg('k' || g, g) || jsonb_build_object('arr',
		 jsonb_agg(repeat('x', g))) AS j
	  FROM generate_series(1, 100) g) s;

-- Refused: another version, too short, trailing bytes, keys out of order,
-- a key twice, a string that isn't UTF-8, padding that isn't zeros, an
-- entry outside its container, a header of neither kind.  The keys "a"
-- and "b" are the document's bytes 22 and 23 (from 1), the padding before
-- 1.5 its bytes 24 and 25.
\set doc '\'{"a": 1.5, "b": 2}\'::jsonb'
SELECT prolly_canonical_value(:doc) AS canon \gset
SELECT encode(:'canon'::bytea, 'hex') AS canon_hex;
SELECT prolly_canonical_decode(:'canon', NULL::jsonb) AS whole;
SELECT prolly_canonical_decode(set_byte(:'canon'::bytea, 0, 2), NULL::jsonb);
SELECT prolly_canonical_decode('\x01'::bytea, NULL::jsonb);
SELECT prolly_canonical_decode('\x0101'::bytea, NULL::jsonb);
SELECT prolly_canonical_decode(:'canon'::bytea || '\x00'::bytea, NULL::jsonb);
SELECT prolly_canonical_decode(overlay(:'canon'::bytea PLACING
		convert_to('ba', 'UTF8') FROM 22 FOR 2), NULL::jsonb);
SELECT prolly_canonical_decode(overlay(:'canon'::bytea PLACING
		convert_to('aa', 'UTF8') FROM 22 FOR 2), NULL::jsonb);
SELECT prolly_canonical_decode(set_byte(:'canon'::bytea, 21, 255), NULL::jsonb);
SELECT prolly_canonical_decode(set_byte(:'canon'::bytea, 23, 1), NULL::jsonb);
SELECT prolly_canonical_decode(set_byte(:'canon'::bytea, 5, 99), NULL::jsonb);
SELECT prolly_canonical_decode(set_byte(:'canon'::bytea, 4, 99), NULL::jsonb);

-- A versioned table's documents go through a commit and back.
CREATE SCHEMA jc VERSIONED;
CREATE TABLE jc.docs (id int PRIMARY KEY, d jsonb);
INSERT INTO jc.docs VALUES (1, '{"n": 1, "tags": ["a", "é"]}'),
	(2, (SELECT jsonb_object_agg('k' || g, repeat('v', g)) FROM generate_series(1, 300) g));
SELECT md5(string_agg(d::text, ',' ORDER BY id)) AS before FROM jc.docs \gset
SELECT length(dolt_commit('-Am', 'documents')::text) AS committed;
UPDATE jc.docs SET d = '{"changed": true}';
SELECT length(dolt_reset('--hard')::text) AS reset;
SELECT md5(string_agg(d::text, ',' ORDER BY id)) = :'before' AS restored FROM jc.docs;
DROP SCHEMA jc CASCADE;

-- A LATIN1 database gives a document the same bytes, though its keys sort
-- otherwise there ("é" is one byte in LATIN1, two in UTF-8).
\set latin_doc '\'{"é": 1, "ab": [2, "ü"], "z": {"é": null}}\'::jsonb'
SELECT prolly_canonical_value(:latin_doc) AS utf8_canon,
	   prolly_canonical_value('{"a": "€"}'::jsonb) AS euro \gset
CREATE DATABASE jc_latin1 ENCODING 'LATIN1' LC_COLLATE 'C' LC_CTYPE 'C'
  TEMPLATE template0;
\c jc_latin1
SET client_encoding = 'UTF8';
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SELECT jsonb_object_keys(:latin_doc) AS latin1_key_order;
SELECT prolly_canonical_value(:latin_doc) = :'utf8_canon' AS same_bytes,
	   prolly_canonical_round_trip(:latin_doc) = :latin_doc AS round_trip,
	   prolly_canonical_decode(:'utf8_canon', NULL::jsonb) = :latin_doc AS decoded;
-- A string LATIN1 can't hold isn't decoded into a wrong one.
SELECT prolly_canonical_decode(:'euro', NULL::jsonb);
\c contrib_regression
DROP DATABASE jc_latin1;
