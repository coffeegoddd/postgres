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
DROP TABLE toast_identity_heap;
