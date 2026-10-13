--
-- Semantics: basic storage.  Rows of every common type, large values, COPY,
-- updates, deletes and TRUNCATE behave exactly as on heap.  The expected
-- output is heap's.
--
SET timezone = 'UTC';
SET extra_float_digits = 0;

-- Tables are created by the engine under test.
CREATE TABLE basic (id int);
SELECT a.amname = current_setting('default_table_access_method') AS uses_engine
FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.relname = 'basic';
DROP TABLE basic;

CREATE TYPE basic_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TYPE basic_pair AS (x int4, y text);
CREATE DOMAIN basic_pos AS int4 CHECK (VALUE > 0);

CREATE TABLE types (
	id int4,
	i2 int2, i8 int8, num numeric(12, 3), f8 float8, b bool,
	t text, vc varchar(10), ch char(3),
	d date, ts timestamptz, iv interval, u uuid, by bytea,
	js json, jb jsonb, ia int4[], ta text[],
	mood basic_mood, pair basic_pair, pos basic_pos,
	ip inet, pt point
);
INSERT INTO types VALUES
	(1, 1, 10000000000, 123.456, 1.5, true,
	 'one', 'v1', 'c1',
	 '2026-01-02', '2026-01-02 03:04:05+00', '1 day 2 hours', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11', '\x0001ff',
	 '{"k": [1, 2]}', '{"k": [1, 2]}', '{1,2,3}', '{a,NULL,c}',
	 'happy', ROW(1, 'x'), 5,
	 '192.168.0.1/24', '(1,2)'),
	(2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
	(3, -32768, -9223372036854775808, -0.001, 'NaN', false,
	 '', '', '', '-infinity', 'infinity', '-1 mon', '00000000-0000-0000-0000-000000000000', '\x',
	 '[]', '[]', '{}', '{}',
	 'sad', ROW(NULL, NULL), 1,
	 '::1', '(0,0)');
SELECT * FROM types ORDER BY id;

-- Domain constraints are still enforced.
INSERT INTO types (id, pos) VALUES (4, 0);

-- Large values (out-of-line storage).
CREATE TABLE big (id int, payload text, blob bytea);
INSERT INTO big
	SELECT g, repeat(md5(g::text), 40000), decode(repeat(md5(g::text), 1000), 'hex')
	FROM generate_series(1, 5) g;
SELECT id, length(payload), md5(payload), length(blob), md5(blob)
FROM big ORDER BY id;
UPDATE big SET payload = payload || 'x' WHERE id = 3;
SELECT id, length(payload), md5(payload) FROM big WHERE id = 3;

-- Many rows; updates and deletes with RETURNING.
CREATE TABLE many (id int, v int, note text);
INSERT INTO many SELECT g, g % 97, 'row ' || g FROM generate_series(1, 20000) g;
SELECT count(*), sum(id), sum(v), count(DISTINCT note) FROM many;
WITH u AS (UPDATE many SET v = v + 1000 WHERE id % 10 = 0 RETURNING id, v)
SELECT * FROM u ORDER BY id LIMIT 3;
WITH u AS (UPDATE many SET v = -1 WHERE id BETWEEN 100 AND 105 RETURNING *)
SELECT * FROM u ORDER BY id;
WITH d AS (DELETE FROM many WHERE id % 3 = 0 RETURNING id)
SELECT count(*), sum(id) FROM d;
SELECT count(*), sum(id), sum(v) FROM many;
-- An update that changes nothing still succeeds.
UPDATE many SET v = v WHERE id = 1;
SELECT * FROM many WHERE id = 1;

-- COPY in and out.
CREATE TABLE copied (id int, t text, n numeric);
COPY copied FROM stdin;
1	one	1.5
2	\N	2.5
3	three	\N
\.
COPY (SELECT * FROM copied ORDER BY id) TO stdout;
COPY (SELECT * FROM copied ORDER BY id DESC) TO stdout WITH (FORMAT csv, HEADER);

-- TRUNCATE, then the table is usable again.
TRUNCATE many;
SELECT count(*) FROM many;
INSERT INTO many VALUES (1, 1, 'again');
SELECT * FROM many;

-- Rollback leaves nothing behind.
BEGIN;
INSERT INTO copied VALUES (99, 'gone', 0);
ROLLBACK;
SELECT count(*) FROM copied WHERE id = 99;

-- Table without columns; table with many columns.
CREATE TABLE nocols ();
INSERT INTO nocols DEFAULT VALUES;
INSERT INTO nocols DEFAULT VALUES;
SELECT count(*) FROM nocols;
SELECT format('CREATE TABLE wide (%s)',
			  string_agg(format('c%s int', g), ', ')) FROM generate_series(1, 300) g \gexec
INSERT INTO wide (c1, c150, c300) VALUES (1, 150, 300);
SELECT c1, c2, c150, c300 FROM wide;

DROP TABLE types, big, many, copied, nocols, wide;
DROP TYPE basic_pair, basic_mood;
DROP DOMAIN basic_pos;
