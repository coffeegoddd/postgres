-- The canonical format of table data is frozen: the hash (SHA-256), the
-- key encoding, the row encoding and the node format.  The same rows give
-- these trees, byte for byte, in every database and every later version;
-- a hash here that changes is a change of format, which needs a new format
-- version, not a new expected value.  Keys of each kind of encoding, rows
-- of every kind of value, special and non-ASCII values among them.
--
-- (Keys here are ordered by the "C" collation, whose order depends on no
-- library.  Only data trees are pinned: the canonical root's other items
-- carry tables' definitions as Postgres's deparser prints them, which
-- isn't part of the frozen format, and a folded root keeps no index trees,
-- a table's indexes being part of its definition.  Non-ASCII text is made
-- from its UTF-8 bytes, so any server encoding that has it reads it the
-- same.)

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
-- A table without a primary key: its rows keyed by the first 32 bytes of
-- their encoded values (after the row's column count and NULLs), then
-- their content's hash: format 2 of folded roots ("!:format"), which a
-- root keyed by the hash alone, as before, lacks.
CREATE TABLE frozen.keyless (a int, b text);
INSERT INTO frozen.keyless SELECT g % 10, 'same' FROM generate_series(1, 500) g;

-- Keys sent in binary (numeric, float, timestamps, uuid, bytea, date) and
-- bool keys.
CREATE TABLE frozen.sent_keys (
	n numeric, f float8, ts timestamp, tz timestamptz, u uuid, bs bytea, d date,
	b bool, v int,
	PRIMARY KEY (n, f, ts, tz, u, bs, d, b));
INSERT INTO frozen.sent_keys
SELECT (g * 1.5)::numeric, g / 4.0, '2026-01-01 00:00'::timestamp + g * interval '1 day',
	'2026-01-01 00:00+00'::timestamptz + g * interval '1 hour', md5('k' || g)::uuid,
	decode(lpad(to_hex(g * 7), 6, '0'), 'hex'), '2000-01-01'::date + g, g % 3 = 0, g
FROM generate_series(1, 500) g;
CREATE TABLE frozen.bool_keys (b bool PRIMARY KEY, v text);
INSERT INTO frozen.bool_keys VALUES (false, 'no'), (true, 'yes');

-- Special values, as keys and as values: NaN, signed zeros and
-- infinities, and non-ASCII text.
CREATE TABLE frozen.special (f8 float8 PRIMARY KEY, f4 float4, n numeric, t text);
INSERT INTO frozen.special VALUES
	('NaN', 'NaN', 'NaN', 'nan'),
	('Infinity', '-Infinity', 'Infinity', 'inf'),
	('-Infinity', 'Infinity', '-Infinity', '-inf'),
	('-0', '-0', '-0', 'minus zero'),
	('1e300', '1e-30', '1e-300', 'tiny');
CREATE TABLE frozen.special_n (n numeric PRIMARY KEY, f8 float8);
INSERT INTO frozen.special_n VALUES ('NaN', 'NaN'), ('Infinity', 'Infinity'),
	('-Infinity', '-Infinity'), ('-0.000', '-0'), ('123.4500', '0');
CREATE TABLE frozen.non_ascii (k text COLLATE "C" PRIMARY KEY, v text, j jsonb);
INSERT INTO frozen.non_ascii
SELECT convert_from(decode(h, 'hex'), 'UTF8'), convert_from(decode(h || '21', 'hex'), 'UTF8'),
	jsonb_build_object(convert_from(decode(h, 'hex'), 'UTF8'), g)
FROM unnest(ARRAY['c3a9', 'c39f', 'c3bc', 'c384', 'c3a96c6cc3a8', '6e61c3af7665']) WITH ORDINALITY u(h, g);

SELECT name, tree, unreadable FROM test_fold_trees()
WHERE name LIKE 'frozen.%' ORDER BY name;

DROP SCHEMA frozen CASCADE;
