-- A type without binary send and receive (an extension's) is kept in a
-- versioned table as its text form, which must read back as the same value:
-- cube before 1.5 and seg have none.  cube's text form is exact, so its
-- values commit, branch, merge, move and key a table; a seg whose text form
-- drops digits is refused where it would be written, and nothing else is
-- disturbed.  (In a database of its own, so the branches here are the only
-- ones.)

CREATE DATABASE regress_text_types;
\c regress_text_types
CREATE SCHEMA tt VERSIONED;
CREATE EXTENSION cube VERSION '1.4' SCHEMA tt;
CREATE EXTENSION seg SCHEMA tt;
SET search_path = tt, public;
SELECT typname, typsend <> 0 AS binary FROM pg_type
 WHERE typname IN ('cube', 'seg') ORDER BY 1;

CREATE DOMAIN tt.box_d AS tt.cube CHECK (tt.cube_dim(VALUE) <= 3);
CREATE TYPE tt.pair AS (a tt.cube, b tt.seg);
CREATE TABLE tt.shapes (id int PRIMARY KEY, c tt.cube, cs tt.cube[], d tt.box_d,
						p tt.pair, s tt.seg);
INSERT INTO tt.shapes VALUES
  (1, '(1,2),(3,4)', ARRAY['(1)', '(2,3)']::tt.cube[], '(1,1,1)', ROW('(5)', '1 .. 2'), '1.5 .. 2.5'),
  (2, '(0.1, 1.2345678901234567)', NULL, NULL, NULL, '<0.1'),
  (3, NULL, '{}', '(0,0)', ROW(NULL, '6.02e23'), NULL);
SELECT length(dolt_commit('-Am', 'text-form types')::text) AS committed;

-- read back from the commit: a new branch, and the table made again from it
SELECT dolt_checkout('-b', 'tt_b');
SELECT id, c, cs, d, p, s FROM tt.shapes ORDER BY id;
SELECT dolt_checkout('main');
DROP TABLE tt.shapes;
SELECT dolt_reset('--hard');
SELECT id, c, cs, d, p, s FROM tt.shapes ORDER BY id;

-- changes, diffs and a merge
UPDATE tt.shapes SET c = '(9,9)' WHERE id = 1;
SELECT diff_type, (from_row).c::text AS from_c, (to_row).c::text AS to_c
  FROM dolt_diff('HEAD', 'WORKING', NULL::tt.shapes);
SELECT length(dolt_commit('-Am', 'main moves 1')::text) AS committed;
SELECT dolt_checkout('tt_b');
UPDATE tt.shapes SET s = '3 .. 4' WHERE id = 2;
SELECT length(dolt_commit('-Am', 'tt_b changes 2')::text) AS committed;
SELECT dolt_checkout('main');
SELECT length(dolt_merge('tt_b')::text) > 0 AS merged;
SELECT id, c, s FROM tt.shapes ORDER BY id;

-- a primary key of such a type
CREATE TABLE tt.keyed (k tt.cube PRIMARY KEY, n int);
INSERT INTO tt.keyed VALUES ('(2)', 2), ('(1)', 1), ('(1,2)', 3);
SELECT length(dolt_commit('-Am', 'keyed by cube')::text) AS committed;
DROP TABLE tt.keyed;
SELECT dolt_reset('--hard');
SELECT k, n FROM tt.keyed ORDER BY k;
SELECT k, n FROM tt.keyed WHERE k = '(1,2)';

-- a value whose text form doesn't read back is refused where it's written
INSERT INTO tt.shapes (id, s) VALUES (10, '1.23456789');
UPDATE tt.shapes SET p = ROW(NULL, '1.23456789') WHERE id = 1;
SELECT count(*) AS rows FROM tt.shapes;
SELECT table_name, status FROM dolt_status() ORDER BY 1;	-- nothing left behind

\c contrib_regression
DROP DATABASE regress_text_types;
