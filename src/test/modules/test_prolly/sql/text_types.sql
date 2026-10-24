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
SELECT dolt_hashof('HEAD') AS before_keys \gset
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

-- the text form is written with fixed settings: a session's
-- extra_float_digits changes nothing a commit holds
CREATE TABLE tt.f (id int PRIMARY KEY, c tt.cube);
INSERT INTO tt.f VALUES (1, '(5e-324)'), (2, '(0.12345678901234568)');
SELECT length(dolt_commit('-Am', 'denormal and 17 digits')::text) AS committed;
SET extra_float_digits = 0;
UPDATE tt.f SET id = id;
SELECT table_name, status FROM dolt_status() ORDER BY 1;
RESET extra_float_digits;
SELECT table_name, status FROM dolt_status() ORDER BY 1;

-- TimeZone, the zone abbreviations, the locale's money, numbers and dates
-- and bytea_output are fixed too (UTC, Default, C, hex): a type printing a
-- time with its zone's offset, or the settings it prints under, has one
-- canonical form whatever the committer's session.  (The types outside the
-- versioned schemas, as an extension's would be.)
CREATE SCHEMA ttx;
SET client_min_messages = warning;
CREATE FUNCTION ttx.canon(anyelement) RETURNS bytea
AS '$libdir/test_prolly', 'prolly_canonical_value' LANGUAGE C STRICT;
CREATE TYPE ttx.stamp;
CREATE FUNCTION ttx.stamp_in(cstring, oid, int4) RETURNS ttx.stamp
AS 'timestamptz_in' LANGUAGE internal STABLE STRICT;
CREATE FUNCTION ttx.stamp_out(ttx.stamp) RETURNS cstring
AS 'timestamptz_out' LANGUAGE internal STABLE STRICT;
CREATE TYPE ttx.stamp (INPUT = ttx.stamp_in, OUTPUT = ttx.stamp_out,
    INTERNALLENGTH = 8, PASSEDBYVALUE, ALIGNMENT = double);
CREATE TYPE ttx.shown;
CREATE FUNCTION ttx.shown_in(cstring) RETURNS ttx.shown
AS '$libdir/test_prolly', 'test_settings_in' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION ttx.shown_out(ttx.shown) RETURNS cstring
AS '$libdir/test_prolly', 'test_settings_out' LANGUAGE C STABLE STRICT;
CREATE TYPE ttx.shown (INPUT = ttx.shown_in, OUTPUT = ttx.shown_out,
    INTERNALLENGTH = variable, STORAGE = extended);
RESET client_min_messages;
SET TimeZone = 'America/New_York';
SET timezone_abbreviations = 'India';
SET lc_monetary = 'POSIX';
SET lc_numeric = 'POSIX';
SET lc_time = 'POSIX';
SET bytea_output = 'escape';
CREATE TABLE tt.z (id int PRIMARY KEY, w ttx.stamp, s ttx.shown);
INSERT INTO tt.z VALUES (1, '2026-01-02 03:04:05.678+00', 'v');
SELECT id, convert_from(substr(ttx.canon(w), 2), 'UTF8') AS w_form,
       convert_from(substr(ttx.canon(s), 2), 'UTF8') AS s_form FROM tt.z;
SELECT md5(ttx.canon(w)) AS w_hash, md5(ttx.canon(s)) AS s_hash FROM tt.z \gset
SELECT length(dolt_commit('-Am', 'settings-dependent text forms')::text) AS committed;
SET TimeZone = 'Asia/Tokyo';
RESET timezone_abbreviations;
SET lc_monetary = 'C';
SET lc_numeric = 'C';
SET lc_time = 'C';
RESET bytea_output;
SELECT md5(ttx.canon(w)) = :'w_hash' AS same_w, md5(ttx.canon(s)) = :'s_hash' AS same_s
  FROM tt.z;
UPDATE tt.z SET id = id;
SELECT table_name, status FROM dolt_status() ORDER BY 1;
DROP TABLE tt.z;
SELECT dolt_reset('--hard');
SELECT id, w::text, s::text FROM tt.z;

-- many values are written with the settings fixed once (a fold's batch):
-- an error there leaves the session's settings as they were, and a value
-- encoded after it fixes them again
SET TimeZone = 'America/New_York';
UPDATE tt.z SET s = 'w';
SET ttx.fail_output = on;
DO $$ BEGIN PERFORM dolt_commit('-Am', 'fails');
EXCEPTION WHEN OTHERS THEN RAISE NOTICE 'caught: %', SQLERRM; END $$;
SHOW TimeZone;
RESET ttx.fail_output;
SELECT convert_from(substr(ttx.canon(s), 2), 'UTF8') AS s_form FROM tt.z;
SELECT length(dolt_commit('-Am', 'w')::text) AS committed;
SET TimeZone = 'Asia/Tokyo';
UPDATE tt.z SET id = id;
SELECT table_name, status FROM dolt_status() ORDER BY 1;
RESET TimeZone;
RESET lc_monetary;
RESET lc_numeric;
RESET lc_time;

-- a column added with a default no row is written with: checked too
ALTER TABLE tt.f ADD COLUMN s tt.seg DEFAULT '1.23456789';
ALTER TABLE tt.f ADD COLUMN s tt.seg DEFAULT '1.5';
SELECT id, s FROM tt.f ORDER BY id;
SELECT length(dolt_commit('-Am', 'a seg column')::text) AS committed;

-- a range over such a type is kept as its text form too
CREATE TYPE tt.segr AS RANGE (subtype = tt.seg);
CREATE TABLE tt.r (id int PRIMARY KEY, r tt.segr);
INSERT INTO tt.r VALUES (1, tt.segr('1', '2'));
INSERT INTO tt.r VALUES (2, tt.segr('1.23456789', '2'));
SELECT length(dolt_commit('-Am', 'a range of seg')::text) AS committed;
DROP TABLE tt.r;
SELECT dolt_reset('--hard');
SELECT id, r FROM tt.r;

-- A type that gains binary send and receive (cube's update to 1.5):
-- values retain their recorded form.  Historical keyed rows are re-sorted
-- under the installed comparator; unrelated history remains readable.
SELECT dolt_hashof('HEAD') AS before_update \gset
ALTER EXTENSION cube UPDATE TO '1.5';
SELECT typname, typsend <> 0 AS binary FROM pg_type WHERE typname = 'cube';
SELECT k, n FROM dolt_at(:'before_update', NULL::tt.keyed) ORDER BY k;
SELECT id, c FROM dolt_at(:'before_keys', NULL::tt.shapes) ORDER BY id;
SELECT table_name, status FROM dolt_status() WHERE table_name IN ('tt.f', 'tt.keyed') ORDER BY 1;
SELECT length(dolt_commit('-Am', 'cube 1.5', '--allow-empty')::text) AS committed;
SELECT dolt_hashof('HEAD') AS after_update \gset
DROP TABLE tt.f, tt.keyed;
SELECT dolt_reset('--hard', :'before_update');
SELECT to_regclass('tt.keyed') IS NULL AS untouched;
SELECT dolt_reset('--hard', :'after_update');
SELECT id, c FROM tt.f ORDER BY id;
SELECT k, n FROM tt.keyed WHERE k = '(1,2)';
SELECT k, n FROM tt.keyed ORDER BY k;
UPDATE tt.keyed SET n = n + 10 WHERE k = '(2)';
SELECT diff_type, (from_row).k::text AS k, (from_row).n AS from_n, (to_row).n AS to_n
  FROM dolt_diff(:'before_update', 'WORKING', NULL::tt.keyed) ORDER BY 2;
SELECT diff_type, (from_row).k::text AS k, (from_row).n AS from_n, (to_row).n AS to_n
  FROM dolt_diff(:'after_update', 'WORKING', NULL::tt.keyed) ORDER BY 2;

\c contrib_regression
DROP DATABASE regress_text_types;
