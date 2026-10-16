-- The canonical format is frozen: the hash (SHA-256), the key encoding,
-- the row encoding and the node format.  The same rows give these trees,
-- byte for byte, in every database and every later version; a hash here
-- that changes is a change of format, which needs a new format version,
-- not a new expected value.  (Keys here are ordered by the "C" collation,
-- whose order depends on no library; tables' definitions are described as
-- Postgres's deparser prints them, which isn't part of the frozen format.)

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

-- The hash.
SELECT test_chunk_put('prolly format') = encode(sha256('prolly format'::bytea), 'hex')
	AS chunks_named_by_sha256;

CREATE SCHEMA frozen VERSIONED;
CREATE TYPE frozen.pair AS (x int, y text);
CREATE TABLE frozen.scalars (
	k8 int8, k4 int4, k2 int2, kt text COLLATE "C",
	b bool, f4 float4, f8 float8, n numeric, t text, bs bytea,
	d date, ts timestamp, tz timestamptz, iv interval, u uuid, j jsonb,
	PRIMARY KEY (k8, k4, k2, kt));
INSERT INTO frozen.scalars
SELECT g, -g, (g % 100)::int2, 'key ' || g,
	g % 2 = 0, g / 3.0, g / 7.0, (g * 1.25)::numeric, repeat('x', g % 50), decode(lpad(to_hex(g), 8, '0'), 'hex'),
	'2026-01-01'::date + g, '2026-01-01 00:00'::timestamp + g * interval '1 minute',
	'2026-01-01 00:00+00'::timestamptz + g * interval '1 second', g * interval '1 hour',
	md5(g::text)::uuid, jsonb_build_object('g', g, 'odd', g % 2 = 1)
FROM generate_series(1, 2000) g;
INSERT INTO frozen.scalars (k8, k4, k2, kt) VALUES (0, 0, 0, '');	-- nulls
CREATE TABLE frozen.composite (id int PRIMARY KEY, p frozen.pair, a int[], ta text[]);
INSERT INTO frozen.composite
SELECT g, ROW(g, 'p' || g)::frozen.pair, ARRAY[g, g + 1, NULL], ARRAY['a', NULL, 'c' || g]
FROM generate_series(1, 300) g;
CREATE TABLE frozen.keyless (a int, b text);
INSERT INTO frozen.keyless SELECT g % 10, 'same' FROM generate_series(1, 500) g;

SELECT test_fold_tree_hash('frozen.scalars') AS scalars,
	test_fold_tree_hash('frozen.composite') AS composite,
	test_fold_tree_hash('frozen.keyless') AS keyless;

DROP SCHEMA frozen CASCADE;
