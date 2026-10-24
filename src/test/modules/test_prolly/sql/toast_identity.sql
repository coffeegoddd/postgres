-- An out-of-line value's pointer identity (toast_pointer_identity(),
-- access/detoast.h), which an extension can key its cache of detoasted
-- values by: PostGIS keeps the geometry a function was last given, so a
-- nested loop probing one polygon per outer row detoasts it once.  A hash
-- pointer's identity is its content's: equal values share it, in any row
-- of any table.  An on-disk pointer's is its TOAST relation and value ID:
-- each stored value its own.  Any other value has none.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE TEMP TABLE v AS
SELECT string_agg(md5(g::text), '') AS a,
	   string_agg(md5((g + 1)::text), '') AS b
FROM generate_series(1, 3000) g;

-- hash pointers: the same value, the same identity; another, another
SELECT test_toast_identity(test_toast_hash_pointer(a)) =
	   test_toast_identity(test_toast_hash_pointer(a)) AS same_value_same,
	   test_toast_identity(test_toast_hash_pointer(a)) <>
	   test_toast_identity(test_toast_hash_pointer(b)) AS other_value_other,
	   octet_length(test_toast_identity(test_toast_hash_pointer(a))) AS length,
	   test_toast_identity(a || '') IS NULL AS in_memory_none,
	   test_toast_identity('short'::text) IS NULL AS inline_none,
	   test_toast_identity(1) IS NULL AS fixed_length_none
FROM v;

-- on-disk pointers: each value stored its own, whatever it holds; read
-- again, the same
CREATE TABLE toast_identity_heap (id int, t text) USING heap;
ALTER TABLE toast_identity_heap ALTER COLUMN t SET STORAGE EXTERNAL;
INSERT INTO toast_identity_heap SELECT 1, a FROM v;
INSERT INTO toast_identity_heap SELECT 2, a FROM v;
SELECT count(DISTINCT test_toast_identity(t)) AS identities,
	   count(test_toast_identity(t)) AS pointers
FROM toast_identity_heap;
SELECT (SELECT test_toast_identity(t) FROM toast_identity_heap WHERE id = 1) =
	   (SELECT test_toast_identity(t) FROM toast_identity_heap WHERE id = 1) AS stable;
-- an on-disk pointer's identity is never a hash pointer's
SELECT (SELECT test_toast_identity(t) FROM toast_identity_heap WHERE id = 1) <>
	   test_toast_identity(test_toast_hash_pointer(a)) AS kinds_differ
FROM v;
-- and a TOAST relation with 8-byte value IDs (VARTAG_ONDISK_OID8): its
-- tag in the identity, another kind's
CREATE TABLE toast_identity_oid8 (t text) USING heap WITH (toast_value_type = 'oid8');
ALTER TABLE toast_identity_oid8 ALTER COLUMN t SET STORAGE EXTERNAL;
INSERT INTO toast_identity_oid8 SELECT a FROM v;
SELECT get_byte(test_toast_identity(o.t), 0) <> get_byte(test_toast_identity(h.t), 0) AS oid8_tag,
	   test_toast_identity(o.t) <> test_toast_identity(h.t) AS oid8_other
FROM toast_identity_oid8 o, toast_identity_heap h WHERE h.id = 1;
DROP TABLE toast_identity_oid8;
DROP TABLE toast_identity_heap;

-- a value compressed in line has none
CREATE TABLE toast_identity_inline (t text) USING heap;
ALTER TABLE toast_identity_inline ALTER COLUMN t SET STORAGE MAIN;
INSERT INTO toast_identity_inline VALUES (repeat('abc', 2000));
SELECT pg_column_compression(t) IS NOT NULL AS compressed,
	   test_toast_identity(t) IS NULL AS compressed_none
FROM toast_identity_inline;
DROP TABLE toast_identity_inline;

-- nor has an expanded value (a PL/pgSQL array variable)
DO $$
DECLARE
	arr int[] := array_fill(1, ARRAY[1000]);
BEGIN
	arr[1] := 2;
	RAISE NOTICE 'expanded none: %', test_toast_identity(arr) IS NULL;
END
$$;

-- rows of versioned tables: one value in several rows of two tables has
-- one identity, its content's
CREATE SCHEMA ti VERSIONED;
CREATE TABLE ti.one (id int PRIMARY KEY, t text);
CREATE TABLE ti.two (id int PRIMARY KEY, t text);
INSERT INTO ti.one SELECT g, a FROM v, generate_series(1, 3) g;
INSERT INTO ti.two SELECT g, a FROM v, generate_series(1, 2) g;
SELECT count(*) AS rows, count(DISTINCT test_toast_identity(t)) AS identities,
	   bool_and(test_toast_identity(t) =
				(SELECT test_toast_identity(test_toast_hash_pointer(a)) FROM v)) AS the_contents
FROM (SELECT t FROM ti.one UNION ALL SELECT t FROM ti.two) r;
DROP SCHEMA ti CASCADE;

-- A hash pointer's identity holds its form: in a LATIN1 database a text
-- of ASCII and its UTF-8 bytes as bytea are one tree, but not one value.
CREATE DATABASE regress_toast_identity_latin1 ENCODING 'LATIN1'
	LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
\c regress_toast_identity_latin1
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE TEMP TABLE v AS SELECT string_agg(md5(g::text), '') AS a FROM generate_series(1, 3000) g;
SELECT (test_toast_pointer(test_toast_hash_pointer(a))).root =
	   (test_toast_pointer(test_toast_hash_pointer(convert_to(a, 'UTF8')))).root AS one_tree,
	   test_toast_identity(test_toast_hash_pointer(a)) <>
	   test_toast_identity(test_toast_hash_pointer(convert_to(a, 'UTF8'))) AS two_identities
FROM v;
\c contrib_regression
DROP DATABASE regress_toast_identity_latin1;
