-- A fold keeps a table's tree from its baseline only while its rows encode
-- as they did then.  What the catalog changes about them without changing
-- a page (a column dropped and one like it added, an enum's label renamed,
-- a composite's attribute dropped) folds the table again; and a value in a
-- TOAST relation isn't given the reference its type's old definition made.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA rr VERSIONED;
CREATE FUNCTION pg_temp.same_as_full() RETURNS bool
AS $$ SELECT test_fold_root() = test_fold_root(true) $$ LANGUAGE sql;

-- A column dropped and one of its name and type added: its values NULL,
-- with a primary key and without.
CREATE TABLE rr.keyed (id int PRIMARY KEY, x int);
CREATE TABLE rr.keyless (a int, x int);
INSERT INTO rr.keyed VALUES (1, 10), (2, 20);
INSERT INTO rr.keyless VALUES (1, 10), (1, 10), (2, 20);
SELECT length(dolt_commit('-Am', 'x')::text) AS committed;
ALTER TABLE rr.keyed DROP COLUMN x;
ALTER TABLE rr.keyed ADD COLUMN x int;
ALTER TABLE rr.keyless DROP COLUMN x;
ALTER TABLE rr.keyless ADD COLUMN x int;
SELECT pg_temp.same_as_full();
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT length(dolt_commit('-Am', 'x again')::text) AS committed;
SELECT dolt_branch('rr_again');
UPDATE rr.keyed SET x = 99;
UPDATE rr.keyless SET x = 99;
SELECT length(dolt_commit('-am', 'x set')::text) AS committed;
SELECT prolly_root_hash('HEAD') = test_fold_root(true) AS as_working;
SELECT dolt_checkout('rr_again');
SELECT * FROM rr.keyed ORDER BY id;
SELECT * FROM rr.keyless ORDER BY a;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'rr_again');

-- An enum's label renamed, used in a column, an array, and a composite.
CREATE TYPE rr.en AS ENUM ('a', 'b');
CREATE TYPE rr.co AS (x int, y text);
CREATE TYPE rr.nest AS (e rr.en, c rr.co);
CREATE TABLE rr.typed (id int PRIMARY KEY, e rr.en, es rr.en[], c rr.co, n rr.nest);
INSERT INTO rr.typed VALUES (1, 'a', '{a,b}', '(1,q)', '(a,"(2,r)")'),
	(2, 'b', '{b}', '(3,s)', '(b,"(4,t)")');
SELECT length(dolt_commit('-Am', 'typed')::text) AS committed;
ALTER TYPE rr.en RENAME VALUE 'a' TO 'z';
SELECT pg_temp.same_as_full();
SELECT length(dolt_commit('-Am', 'renamed')::text) AS committed;
UPDATE rr.typed SET e = e;				-- (no change)
SELECT count(*) AS clean FROM dolt_status;
SELECT diff_type, (from_row).id, (to_row).id FROM dolt_diff('HEAD', 'WORKING', NULL::rr.typed);
SELECT prolly_root_hash('HEAD') = test_fold_root(true) AS as_working;

-- A composite's attribute dropped (and one nested in another).
ALTER TYPE rr.co DROP ATTRIBUTE y CASCADE;
SELECT pg_temp.same_as_full();
SELECT length(dolt_commit('-Am', 'dropped')::text) AS committed;
UPDATE rr.typed SET c = c;				-- (no change)
SELECT count(*) AS clean FROM dolt_status;
SELECT diff_type, (from_row).id, (to_row).id FROM dolt_diff('HEAD', 'WORKING', NULL::rr.typed);
SELECT prolly_root_hash('HEAD') = test_fold_root(true) AS as_working;

-- Long values of the type, in a TOAST relation (the memo's) and by
-- content (read each fold): one tree, before the rename and after.
CREATE TABLE rr.long_content (id int PRIMARY KEY, es rr.en[], ns rr.nest[]);
SET debug_toast_relation = on;
CREATE TABLE rr.long_toast (LIKE rr.long_content INCLUDING ALL);
ALTER TABLE rr.long_toast ALTER COLUMN es SET STORAGE EXTERNAL,
	ALTER COLUMN ns SET STORAGE EXTERNAL;
INSERT INTO rr.long_toast
SELECT 1, array_agg('b'::rr.en), array_agg(ROW('b', ROW(i))::rr.nest)
FROM generate_series(1, 1000) i;
RESET debug_toast_relation;
INSERT INTO rr.long_content SELECT * FROM rr.long_toast;
SELECT pg_column_toast_chunk_id(es) IS NOT NULL AS es_in_toast,
	   pg_column_toast_chunk_id(ns) IS NOT NULL AS ns_in_toast FROM rr.long_toast;
SELECT count(DISTINCT tree) AS trees FROM test_fold_trees()
WHERE name IN ('rr.long_content', 'rr.long_toast');
SELECT count(DISTINCT tree) AS trees FROM test_fold_trees()	-- (remembered)
WHERE name IN ('rr.long_content', 'rr.long_toast');
ALTER TYPE rr.en RENAME VALUE 'b' TO 'y';
SELECT count(DISTINCT tree) AS trees FROM test_fold_trees()
WHERE name IN ('rr.long_content', 'rr.long_toast');
ALTER TYPE rr.nest DROP ATTRIBUTE c CASCADE;
SELECT count(DISTINCT tree) AS trees FROM test_fold_trees()
WHERE name IN ('rr.long_content', 'rr.long_toast');
SELECT pg_temp.same_as_full();

DROP SCHEMA rr CASCADE;
