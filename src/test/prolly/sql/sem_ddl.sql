--
-- Semantics: DDL on tables that hold data.  Added columns, defaults,
-- dropped columns, type changes and renames behave exactly as on heap.
--
CREATE TABLE evolve (id int PRIMARY KEY, a text);
INSERT INTO evolve SELECT g, 'a' || g FROM generate_series(1, 5) g;

-- A nullable column, then one with a default (old rows get the default).
ALTER TABLE evolve ADD COLUMN b int;
ALTER TABLE evolve ADD COLUMN c int DEFAULT 7;
ALTER TABLE evolve ADD COLUMN d timestamptz DEFAULT '2026-01-01 00:00:00+00';
INSERT INTO evolve (id, a, b) VALUES (6, 'a6', 60);
INSERT INTO evolve (id, a, b, c) VALUES (7, 'a7', 70, NULL);
SELECT id, a, b, c, d IS NOT NULL AS has_d FROM evolve ORDER BY id;

-- Dropping a column hides it; its values don't come back.
ALTER TABLE evolve DROP COLUMN a;
SELECT * FROM evolve ORDER BY id LIMIT 3;
ALTER TABLE evolve ADD COLUMN a text;
SELECT id, a FROM evolve ORDER BY id LIMIT 3;

-- Type change rewrites the table.
ALTER TABLE evolve ALTER COLUMN b TYPE bigint USING coalesce(b, -1) * 10;
SELECT id, b, pg_typeof(b) FROM evolve ORDER BY id;

-- Renames.
ALTER TABLE evolve RENAME COLUMN c TO c2;
ALTER TABLE evolve RENAME TO evolved;
SELECT id, c2 FROM evolved ORDER BY id;

-- NOT NULL added on existing data.
ALTER TABLE evolved ALTER COLUMN c2 SET NOT NULL;
UPDATE evolved SET c2 = 0 WHERE c2 IS NULL;
ALTER TABLE evolved ALTER COLUMN c2 SET NOT NULL;
INSERT INTO evolved (id, c2) VALUES (8, NULL);

-- Generated columns.
CREATE TABLE gen (a int, b int GENERATED ALWAYS AS (a * 2) STORED,
				  c int GENERATED ALWAYS AS (a * 3) VIRTUAL);
INSERT INTO gen (a) VALUES (1), (2), (NULL);
UPDATE gen SET a = 10 WHERE a = 2;
SELECT * FROM gen ORDER BY a NULLS LAST;

-- CREATE TABLE AS, SELECT INTO, and a table copied with LIKE.
CREATE TABLE ctas AS SELECT id, c2 FROM evolved WHERE id <= 3;
SELECT * INTO selinto FROM ctas WHERE id = 2;
CREATE TABLE liked (LIKE evolved INCLUDING ALL);
INSERT INTO liked SELECT * FROM evolved;
SELECT (SELECT count(*) FROM ctas), (SELECT count(*) FROM selinto),
	   (SELECT count(*) FROM liked);

DROP TABLE evolved, gen, ctas, selinto, liked;

-- Dropping the primary key's column, and dropping the key's type with it.
CREATE TABLE dropkey (id int PRIMARY KEY, v text);
INSERT INTO dropkey VALUES (1, 'kept'), (2, 'also kept');
ALTER TABLE dropkey DROP COLUMN id CASCADE;
SELECT * FROM dropkey ORDER BY v;
INSERT INTO dropkey VALUES ('next');
SELECT * FROM dropkey ORDER BY v;
CREATE TYPE mood AS ENUM ('sad', 'happy');
CREATE TABLE dropkeytype (m mood PRIMARY KEY, v text);
INSERT INTO dropkeytype VALUES ('sad', 'one'), ('happy', 'two');
DROP TYPE mood CASCADE;
SELECT * FROM dropkeytype ORDER BY v;
INSERT INTO dropkeytype VALUES ('three');
SELECT * FROM dropkeytype ORDER BY v;
DROP TABLE dropkey, dropkeytype;

-- The key's type dropped while the transaction has rows of its own.
CREATE TYPE mood2 AS ENUM ('sad', 'happy', 'meh');
CREATE TABLE droptype_active (m mood2 PRIMARY KEY, v text);
INSERT INTO droptype_active VALUES ('sad', 'old'), ('meh', 'older');
BEGIN;
INSERT INTO droptype_active VALUES ('happy', 'new');
DROP TYPE mood2 CASCADE;
SAVEPOINT after_drop;
SELECT * FROM droptype_active ORDER BY v;
ROLLBACK TO after_drop;
INSERT INTO droptype_active VALUES ('third');
SELECT * FROM droptype_active ORDER BY v;
COMMIT;
SELECT * FROM droptype_active ORDER BY v;
CREATE TYPE mood3 AS ENUM ('sad', 'happy', 'meh');
CREATE TABLE droptype_commit (m mood3 PRIMARY KEY, v text);
INSERT INTO droptype_commit VALUES ('sad', 'old'), ('meh', 'older');
BEGIN;
INSERT INTO droptype_commit VALUES ('happy', 'new');
DROP TYPE mood3 CASCADE;
COMMIT;
SELECT * FROM droptype_commit ORDER BY v;
DROP TABLE droptype_active, droptype_commit;
